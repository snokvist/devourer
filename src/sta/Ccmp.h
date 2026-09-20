/* Ccmp — 802.11 CCMP framing, shared by the AP and station roles.
 *
 * This is the framing only: AAD construction, nonce construction, the 8-byte
 * CCMP header, and the PN. The cipher itself is a CryptoOps call, so nothing
 * here includes a crypto library and `libdevourer` gains no dependency.
 *
 * Promoted from `tests/ap_wpa2.cpp`, where the same rules were inline and
 * untested. The details that mattered on air are recorded at the functions
 * that implement them, because each one was a debugging session: the AAD masks
 * the FC subtype/retry/pwr-mgmt/more-data bits and sets Protected, and masks
 * the sequence number while KEEPING the fragment number; the nonce is
 * 0 | A2 | PN as six big-endian bytes; and the CCMP header carries the 48-bit
 * PN split across two discontiguous ranges with an ext-IV bit that is not
 * optional.
 *
 * Both roles use the identical framing — an AP encrypting to a station and a
 * station encrypting to its AP differ only in which address is A2 and which
 * PN counter they draw from. That is the whole reason this is one file rather
 * than two.
 */
#ifndef DEVOURER_STA_CCMP_H
#define DEVOURER_STA_CCMP_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sta/CryptoOps.h"

namespace devourer {
namespace sta {

/* An 802.11 CCMP header is 8 bytes and the MIC is 8; a protected MPDU is
 * therefore 16 bytes longer than its plaintext plus the 802.11 header. */
constexpr size_t kCcmpHdrLen = 8;
constexpr size_t kCcmpMicLen = 8;
constexpr size_t kCcmpAadMax = 32;
constexpr size_t kCcmpNonceLen = 13;

/* Additional authenticated data over the 802.11 header (802.11-2016 12.5.3.3.3).
 *
 * The masking is the part that is easy to get subtly wrong and impossible to
 * debug from the other end, because a wrong AAD looks exactly like a wrong key:
 * the MIC simply fails.
 *
 *  - Frame Control: subtype bits, Retry, Pwr Mgmt and More Data are masked out
 *    because they may legitimately change between transmission and reception
 *    (a retransmission sets Retry; a buffering AP sets More Data). Protected
 *    is forced ON because the receiver sees it set.
 *  - Sequence Control: the sequence number is masked, the FRAGMENT number is
 *    kept. Masking both breaks fragmented frames; keeping both breaks every
 *    frame, because the sequence number is assigned after the MIC is computed
 *    on a hardware-sequencing MAC.
 *
 *  - QoS Control: for a QoS data frame the AAD gains two more octets carrying
 *    the TID, everything else masked. `qos_tid` selects that form; -1 is the
 *    non-QoS form. This is NOT cosmetic - a peer that includes the QoS field
 *    while we omit it computes a different MIC, and the frame is dropped with
 *    no diagnostic on either side. The AP harnesses have always used the
 *    non-QoS form and interoperate, so the station side must not assume the
 *    other one works until it is measured against a real AP.
 *
 * `hdr` is a 24-byte 3-address header. Returns the AAD length: 22, or 24 for
 * the QoS form.
 */
inline size_t ccmp_aad(const uint8_t* hdr, uint8_t* aad, int qos_tid = -1) {
  uint16_t fc = (uint16_t)(hdr[0] | (hdr[1] << 8));

  fc &= (uint16_t)~0x0070u;                      /* subtype */
  fc &= (uint16_t)~(0x0800u | 0x1000u | 0x2000u); /* retry, pwr mgmt, more data */
  fc |= 0x4000u;                                  /* protected */
  aad[0] = (uint8_t)(fc & 0xff);
  aad[1] = (uint8_t)(fc >> 8);
  std::memcpy(aad + 2, hdr + 4, 18);              /* addr1, addr2, addr3 */
  {
    uint16_t seq = (uint16_t)((hdr[22] | (hdr[23] << 8)) & 0x000f);

    aad[20] = (uint8_t)(seq & 0xff);
    aad[21] = (uint8_t)(seq >> 8);
  }
  if (qos_tid >= 0) {
    /* 802.11-2016 12.5.3.3.3: the QoS Control octets are included with only
     * the TID retained; the ack-policy, EOSP and A-MSDU bits are masked
     * because they may differ between transmission and reception. */
    aad[22] = (uint8_t)(qos_tid & 0x0f);
    aad[23] = 0;
    return 24;
  }
  return 22;
}

/* CCM nonce: flag octet 0, then A2, then the 48-bit PN BIG-endian
 * (802.11-2016 12.5.3.3.4). Big-endian here and little-endian in the CCMP
 * header below — they genuinely differ, and reusing one for the other is the
 * classic way to produce a frame only your own implementation can read. */
inline void ccmp_nonce(const uint8_t a2[6], uint64_t pn, uint8_t* nonce) {
  nonce[0] = 0;
  std::memcpy(nonce + 1, a2, 6);
  for (int i = 0; i < 6; i++)
    nonce[7 + i] = (uint8_t)((pn >> (8 * (5 - i))) & 0xff);
}

/* The 8-byte CCMP header: PN0, PN1, a reserved byte, then the key-id octet
 * with Ext IV set, then PN2..PN5 little-endian. The PN is deliberately NOT
 * contiguous - byte 2 is reserved and byte 3 is not part of the PN at all. */
inline void ccmp_header(uint64_t pn, uint8_t key_id, uint8_t* out) {
  out[0] = (uint8_t)(pn & 0xff);
  out[1] = (uint8_t)((pn >> 8) & 0xff);
  out[2] = 0;
  out[3] = (uint8_t)(0x20 | ((key_id & 0x03) << 6)); /* Ext IV | KeyID */
  out[4] = (uint8_t)((pn >> 16) & 0xff);
  out[5] = (uint8_t)((pn >> 24) & 0xff);
  out[6] = (uint8_t)((pn >> 32) & 0xff);
  out[7] = (uint8_t)((pn >> 40) & 0xff);
}

/* Recover the 48-bit PN from a received CCMP header. */
inline uint64_t ccmp_header_pn(const uint8_t* ccmp_hdr) {
  return (uint64_t)ccmp_hdr[0] | ((uint64_t)ccmp_hdr[1] << 8) |
         ((uint64_t)ccmp_hdr[4] << 16) | ((uint64_t)ccmp_hdr[5] << 24) |
         ((uint64_t)ccmp_hdr[6] << 32) | ((uint64_t)ccmp_hdr[7] << 40);
}

/* Protect one MPDU.
 *
 * `hdr` is the 24-byte header (its Protected bit need not be set; the AAD
 * forces it and this writes it into the output). `a2` is the transmitter
 * address the nonce is built from - passed separately rather than read from
 * the header because a 4-address frame puts it elsewhere, and silently reading
 * the wrong one would produce frames that only decrypt locally.
 *
 * Writes 24 + 8 + plain_len + 8 bytes to `out`. Returns the length written, or
 * 0 on failure.
 */
inline size_t ccmp_encrypt(CryptoOps& crypto, const uint8_t tk[16],
                           const uint8_t* hdr, const uint8_t a2[6], uint64_t pn,
                           uint8_t key_id, const uint8_t* plain,
                           size_t plain_len, uint8_t* out, int qos_tid = -1) {
  uint8_t aad[kCcmpAadMax];
  uint8_t nonce[kCcmpNonceLen];
  size_t aad_len = ccmp_aad(hdr, aad, qos_tid);

  ccmp_nonce(a2, pn, nonce);
  std::memcpy(out, hdr, 24);
  out[1] |= 0x40; /* Protected, in the frame we actually air */
  ccmp_header(pn, key_id, out + 24);
  if (!crypto.aes_ccm(true, tk, nonce, aad, aad_len, plain, plain_len,
                      out + 24 + kCcmpHdrLen,
                      out + 24 + kCcmpHdrLen + plain_len))
    return 0;
  return 24 + kCcmpHdrLen + plain_len + kCcmpMicLen;
}

/* Unprotect one MPDU. `mpdu` is the whole received frame starting at the
 * 802.11 header, WITHOUT the FCS. On success writes the plaintext to `out`
 * (at most mpdu_len - 40 bytes) and reports its length and the frame's PN.
 *
 * Returns false when the frame is too short, or when the MIC does not verify.
 * A false return means DROP: `out` holds no trustworthy bytes, and the PN must
 * not be admitted to a replay window. Replay checking itself is the caller's -
 * it needs per-TID state this function does not own. */
inline bool ccmp_decrypt(CryptoOps& crypto, const uint8_t tk[16],
                         const uint8_t* mpdu, size_t mpdu_len,
                         const uint8_t a2[6], uint8_t* out, size_t* out_len,
                         uint64_t* pn_out, int qos_tid = -1) {
  const size_t overhead = 24 + kCcmpHdrLen + kCcmpMicLen;
  uint8_t aad[kCcmpAadMax];
  uint8_t nonce[kCcmpNonceLen];
  size_t aad_len, body;
  uint64_t pn;

  if (mpdu_len <= overhead) return false;
  body = mpdu_len - overhead;
  pn = ccmp_header_pn(mpdu + 24);
  aad_len = ccmp_aad(mpdu, aad, qos_tid);
  ccmp_nonce(a2, pn, nonce);
  if (!crypto.aes_ccm(false, tk, nonce, aad, aad_len, mpdu + 24 + kCcmpHdrLen,
                      body, out,
                      const_cast<uint8_t*>(mpdu + 24 + kCcmpHdrLen + body)))
    return false;
  if (out_len) *out_len = body;
  if (pn_out) *pn_out = pn;
  return true;
}

/* Per-key receive replay state (802.11-2016 12.5.3.4.4).
 *
 * The gate is `pn <= last`, NOT `pn < last`. PR #335 shipped the strict form
 * and its review called it out as KRACK-class: an equal-counter replay passed,
 * which is exactly how a group-key reinstallation attack lands. Keep the
 * equality. */
class CcmpReplay {
public:
  /* True when this PN is acceptable AND records it. A rejected PN leaves the
   * window untouched — accepting a frame's PN before its MIC verifies would
   * let an attacker advance the window with garbage. Call this only after a
   * successful decrypt. */
  bool accept(uint64_t pn) {
    if (seen_ && pn <= last_) return false;
    last_ = pn;
    seen_ = true;
    return true;
  }
  void reset() {
    last_ = 0;
    seen_ = false;
  }
  uint64_t last() const { return last_; }

private:
  uint64_t last_ = 0;
  bool seen_ = false;
};

}  // namespace sta
}  // namespace devourer

#endif /* DEVOURER_STA_CCMP_H */
