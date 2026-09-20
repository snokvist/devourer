/* Headless guard for src/sta/Ccmp.h — the 802.11 CCMP framing both the AP and
 * the station roles share.
 *
 * WHAT THIS IS. The vectors in ccmp_vectors.h come from a third
 * implementation: python-cryptography's AESCCM for the cipher, and the 802.11
 * framing transcribed in tests/ccmp_gen_vectors.py straight from
 * 802.11-2016 12.5.3.3, independently of the header under test. So a failure
 * here is two independent readings of the standard disagreeing.
 *
 * WHAT IT IS NOT. Not the official IEEE Annex J vector — that would be
 * strictly better and is a drop-in replacement when someone has it to hand.
 * And it is not a round-trip: the whole reason it exists is that PR #335
 * shipped hand-rolled crypto with zero known-answer tests, and its review
 * named that the single highest-leverage gap in the PR. A round-trip would
 * have passed on that code too.
 *
 * The cipher here is OpenSSL, via the same primitive tests/ap_wpa2.cpp uses,
 * so this also pins the AP harness's crypto to the same answers.
 */
#include <openssl/evp.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "ccmp_software.h"
#include "ccmp_vectors.h"
#include "sta/Ccmp.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_fail++;
  }
}

/* The OpenSSL CryptoOps the harnesses will supply. Only aes_ccm is exercised
 * here; the rest refuse rather than pretend, so a future test that needs them
 * fails loudly instead of silently passing on a stub. */
struct OpenSslCcm : devourer::sta::CryptoOps {
  bool aes_ccm(bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
               const uint8_t* aad, size_t aad_len, const uint8_t* in,
               size_t in_len, uint8_t* out, uint8_t* tag) override {
    return devourer::test::ccmp_software(encrypt, key, nonce, aad, (int)aad_len,
                                         in, (int)in_len, out, tag);
  }
  bool hmac_sha1(const uint8_t*, size_t, const uint8_t*, size_t,
                 uint8_t[20]) override {
    return false;
  }
  bool pbkdf2_sha1(const char*, const uint8_t*, size_t, unsigned, uint8_t*,
                   size_t) override {
    return false;
  }
  bool aes_key_unwrap(const uint8_t*, size_t, const uint8_t*, size_t,
                      uint8_t*) override {
    return false;
  }
};

void test_vectors() {
  OpenSslCcm crypto;

  for (size_t i = 0; i < kCcmpVectorCount; i++) {
    const CcmpVector& v = kCcmpVectors[i];
    std::vector<uint8_t> out(v.mpdu_len + 64, 0);
    char label[128];

    std::snprintf(label, sizeof label, "encrypt vector '%s'", v.name);
    size_t n = devourer::sta::ccmp_encrypt(crypto, v.tk, v.hdr, v.a2, v.pn,
                                           v.key_id, v.plain, v.plain_len,
                                           out.data(), v.qos_tid);
    check(n == v.mpdu_len, label);
    if (n == v.mpdu_len)
      check(std::memcmp(out.data(), v.mpdu, n) == 0, label);

    /* Decrypt the vector's own bytes, not the ones we just produced — a
     * mutual-agreement test between our two directions would pass even if both
     * disagreed with the standard. */
    std::vector<uint8_t> mpdu(v.mpdu, v.mpdu + v.mpdu_len);
    std::vector<uint8_t> plain(v.mpdu_len, 0);
    size_t plain_len = 0;
    uint64_t pn = 0;

    std::snprintf(label, sizeof label, "decrypt vector '%s'", v.name);
    bool ok = devourer::sta::ccmp_decrypt(crypto, v.tk, mpdu.data(),
                                          mpdu.size(), v.a2, plain.data(),
                                          &plain_len, &pn, v.qos_tid);
    check(ok, label);
    if (ok) {
      check(plain_len == v.plain_len, label);
      check(std::memcmp(plain.data(), v.plain, v.plain_len) == 0, label);
      check(pn == v.pn, label);
    }
  }
}

/* A corrupted MIC must be rejected. Without this the decrypt path could ignore
 * the tag entirely and every other test above would still pass. */
void test_mic_rejected() {
  OpenSslCcm crypto;
  const CcmpVector& v = kCcmpVectors[0];
  std::vector<uint8_t> mpdu(v.mpdu, v.mpdu + v.mpdu_len);
  std::vector<uint8_t> plain(v.mpdu_len, 0);

  mpdu[mpdu.size() - 1] ^= 0x01;
  check(!devourer::sta::ccmp_decrypt(crypto, v.tk, mpdu.data(), mpdu.size(),
                                     v.a2, plain.data(), nullptr, nullptr),
        "a flipped MIC bit must be rejected");

  /* Same for the ciphertext: CCM authenticates it, so a body edit must fail
   * the tag too. */
  std::vector<uint8_t> body(v.mpdu, v.mpdu + v.mpdu_len);
  body[24 + devourer::sta::kCcmpHdrLen] ^= 0x80;
  check(!devourer::sta::ccmp_decrypt(crypto, v.tk, body.data(), body.size(),
                                     v.a2, plain.data(), nullptr, nullptr),
        "a flipped ciphertext bit must be rejected");

  /* And a frame shorter than its own overhead must be refused rather than
   * read past its end. */
  check(!devourer::sta::ccmp_decrypt(crypto, v.tk, v.mpdu, 24 + 8 + 8, v.a2,
                                     plain.data(), nullptr, nullptr),
        "a frame with no body must be refused");
}

/* The two AAD forms must not be interchangeable. A QoS frame decrypted with
 * the non-QoS AAD has to fail — that mismatch is the silent one, and if it
 * ever passed it would mean the TID octets were not reaching the MIC at all. */
void test_qos_aad_distinct() {
  OpenSslCcm crypto;
  const CcmpVector* q = nullptr;

  for (size_t i = 0; i < kCcmpVectorCount; i++)
    if (kCcmpVectors[i].qos_tid >= 0) q = &kCcmpVectors[i];
  check(q != nullptr, "there is a QoS vector to test");
  if (!q) return;

  std::vector<uint8_t> plain(q->mpdu_len, 0);
  check(!devourer::sta::ccmp_decrypt(crypto, q->tk, q->mpdu, q->mpdu_len,
                                     q->a2, plain.data(), nullptr, nullptr, -1),
        "a QoS frame must NOT decrypt under the non-QoS AAD");
  check(devourer::sta::ccmp_decrypt(crypto, q->tk, q->mpdu, q->mpdu_len, q->a2,
                                    plain.data(), nullptr, nullptr,
                                    q->qos_tid),
        "the same frame decrypts under the QoS AAD");
  /* And the wrong TID must fail too, or the TID is not really authenticated. */
  check(!devourer::sta::ccmp_decrypt(crypto, q->tk, q->mpdu, q->mpdu_len,
                                     q->a2, plain.data(), nullptr, nullptr,
                                     (q->qos_tid + 1) & 0x0f),
        "a wrong TID must not verify");

  uint8_t aad[devourer::sta::kCcmpAadMax];
  check(devourer::sta::ccmp_aad(q->hdr, aad, 5) == 24,
        "the QoS AAD is 24 bytes");
  check(aad[22] == 5 && aad[23] == 0,
        "the QoS AAD carries the TID with the other bits masked");
}

/* The AAD rules, asserted directly, because they are the part that is silent
 * when wrong: a bad AAD is indistinguishable from a bad key at the far end. */
void test_aad_masking() {
  uint8_t hdr[24];
  uint8_t aad[devourer::sta::kCcmpAadMax];

  std::memset(hdr, 0, sizeof hdr);
  hdr[0] = 0x88;                    /* QoS data, subtype bits set */
  hdr[1] = 0x08 | 0x10 | 0x20;      /* retry | pwr mgmt | more data */
  hdr[22] = 0x35;                   /* frag 5, seq low bits */
  hdr[23] = 0x12;                   /* seq high */

  size_t n = devourer::sta::ccmp_aad(hdr, aad);
  check(n == 22, "AAD is 22 bytes for a 3-address header");
  check((aad[0] & 0x70) == 0, "AAD masks the FC subtype bits");
  check((aad[1] & 0x08) == 0, "AAD masks Retry");
  check((aad[1] & 0x10) == 0, "AAD masks Pwr Mgmt");
  check((aad[1] & 0x20) == 0, "AAD masks More Data");
  check((aad[1] & 0x40) != 0, "AAD forces Protected on");
  check(aad[20] == 0x05, "AAD keeps the fragment number");
  check(aad[21] == 0x00, "AAD masks the sequence number");
}

/* The CCMP header's PN is split across two discontiguous ranges and the Ext IV
 * bit is not optional. Round-tripping the maximum PN catches a 32-bit
 * truncation, which would otherwise only appear after 4 billion frames. */
void test_header_pn() {
  uint8_t h[8];
  const uint64_t pn = 0xfedcba987654ULL;

  devourer::sta::ccmp_header(pn, 2, h);
  check((h[3] & 0x20) != 0, "CCMP header sets Ext IV");
  check(((h[3] >> 6) & 3) == 2, "CCMP header carries the key id");
  check(h[2] == 0, "CCMP header byte 2 is reserved and zero");
  check(devourer::sta::ccmp_header_pn(h) == pn, "48-bit PN round-trips");

  devourer::sta::ccmp_header(0xffffffffffffULL, 0, h);
  check(devourer::sta::ccmp_header_pn(h) == 0xffffffffffffULL,
        "the maximum PN round-trips");
}

/* The replay gate. PR #335 shipped `<` here and its review called it
 * KRACK-class: an equal-counter replay passed, which is how a group-key
 * reinstallation lands. This is the regression test for that specific bug. */
void test_replay() {
  devourer::sta::CcmpReplay r;

  check(r.accept(1), "first PN is accepted");
  check(!r.accept(1), "an EQUAL-counter replay must be REJECTED");
  check(!r.accept(0), "a lower PN must be rejected");
  check(r.accept(2), "a higher PN is accepted");
  check(r.last() == 2, "the window advanced to the accepted PN");
  check(!r.accept(2), "the advanced counter still rejects its equal");
  r.reset();
  check(r.accept(1), "reset clears the window for a rekey");

  /* A rejected PN must not advance the window; otherwise a forged high PN
   * would lock out the legitimate peer. */
  devourer::sta::CcmpReplay r2;
  check(r2.accept(100), "setup");
  check(!r2.accept(50), "setup");
  check(r2.last() == 100, "a rejected PN does not move the window");
}

}  // namespace

int main() {
  test_vectors();
  test_mic_rejected();
  test_qos_aad_distinct();
  test_aad_masking();
  test_header_pn();
  test_replay();

  if (g_fail) {
    std::printf("ccmp_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("ccmp_selftest: OK (%zu vectors)\n", kCcmpVectorCount);
  return 0;
}
