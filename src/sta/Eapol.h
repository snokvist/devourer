/* Eapol — the EAPOL-Key wire format and the key derivations built on it.
 *
 * This is format and arithmetic only: parse, build, derive, verify. The
 * decisions - what to do with a message that arrives in the wrong state,
 * whether a replay counter is acceptable, when to install a key - belong to
 * Supplicant.h, which is where they can be tested as decisions.
 *
 * Everything cryptographic goes through CryptoOps, so `libdevourer` gains no
 * dependency. That is not a stylistic preference: PR #335 shipped hand-rolled
 * AES, CCM, PBKDF2 and the PRF with zero known-answer tests, and its review
 * named that the single highest-leverage gap in six thousand lines.
 *
 * ONLY KEY DESCRIPTOR VERSION 2 (HMAC-SHA1 MIC, AES key wrap, CCMP). Version 1
 * is TKIP - HMAC-MD5 and RC4 - and version 3 is AES-128-CMAC. Neither is
 * implemented, and both are REFUSED rather than treated as version 2, because
 * a version mismatch means the MIC is computed with a different algorithm and
 * "the MIC did not verify" would be the only symptom.
 */
#ifndef DEVOURER_STA_EAPOL_H
#define DEVOURER_STA_EAPOL_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sta/CryptoOps.h"

namespace devourer {
namespace sta {

/* An EAPOL-Key frame is 99 bytes before its key data. The offsets below are
 * 802.11-2016 12.7.2.
 *
 * DO NOT CONVERT tests/ap_wpa2.cpp ONTO THESE. That authenticator hand-rolls
 * every one of these offsets inline, it was written months earlier, and it
 * shares no code with this header - which is exactly what makes
 * `test_library_station_associates()` a real oracle. Two implementations from
 * one set of constants cannot disagree, and cannot catch a misreading either.
 * The duplication is the test. */
inline constexpr size_t kEapolKeyFixedLen = 99;
inline constexpr size_t kEapolMicOff = 81;
inline constexpr size_t kEapolMicLen = 16;
inline constexpr size_t kEapolNonceOff = 17;
inline constexpr size_t kEapolReplayOff = 9;
inline constexpr size_t kEapolRscOff = 65;
inline constexpr size_t kEapolKeyDataLenOff = 97;

/* Key Information bits (802.11-2016 Figure 12-34). */
enum : uint16_t {
  kKiVersionMask = 0x0007,
  kKiPairwise = 0x0008,
  kKiKeyIdMask = 0x0030,
  kKiInstall = 0x0040,
  kKiAck = 0x0080,
  kKiMic = 0x0100,
  kKiSecure = 0x0200,
  kKiError = 0x0400,
  kKiRequest = 0x0800,
  kKiEncrypted = 0x1000,
};

/* The only key descriptor version this implements: HMAC-SHA1-128 MIC and
 * NIST AES key wrap, which is what WPA2-PSK with CCMP uses. */
inline constexpr uint16_t kKeyDescVersionCcmp = 2;
inline constexpr uint8_t kKeyDescTypeRsn = 2;

/* THE 802.1X PROTOCOL VERSION A SUPPLICANT SENDS.
 *
 * One, not two, and not an echo of what the authenticator sent. This is what
 * wpa_supplicant ships as its default, and the reason is compatibility: the
 * octet is inside the MIC'd region, some access points have historically
 * misbehaved on version 2 from a station, and there is no upside to claiming
 * a higher number - the field is not a negotiation.
 *
 * Found by comparing our message 2 with a captured wpa_supplicant one byte
 * for byte. An earlier draft echoed the authenticator's version, which
 * produced 2 against hostapd and was the last difference between the two
 * frames. */
inline constexpr uint8_t kEapolVersionSupplicant = 1;

/* A parsed EAPOL-Key frame. The pointers alias the caller's buffer and are
 * valid only as long as it is. */
struct EapolKey {
  const uint8_t* frame = nullptr;  /* the whole EAPOL frame, from byte 0 */
  size_t frame_len = 0;            /* its true length, key data included */
  uint8_t descriptor = 0;
  uint16_t key_info = 0;
  uint16_t version = 0;
  uint16_t key_len = 0;
  uint64_t replay = 0;
  const uint8_t* nonce = nullptr;  /* 32 bytes */
  const uint8_t* rsc = nullptr;    /* 8 bytes */
  const uint8_t* mic = nullptr;    /* 16 bytes */
  const uint8_t* key_data = nullptr;
  size_t key_data_len = 0;

  bool pairwise() const { return (key_info & kKiPairwise) != 0; }
  bool install() const { return (key_info & kKiInstall) != 0; }
  bool ack() const { return (key_info & kKiAck) != 0; }
  bool has_mic() const { return (key_info & kKiMic) != 0; }
  bool secure() const { return (key_info & kKiSecure) != 0; }
  bool error() const { return (key_info & kKiError) != 0; }
  bool request() const { return (key_info & kKiRequest) != 0; }
  bool encrypted() const { return (key_info & kKiEncrypted) != 0; }
  uint8_t key_id() const { return (uint8_t)((key_info & kKiKeyIdMask) >> 4); }
};

inline uint16_t eapol_be16(const uint8_t* p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

inline uint64_t eapol_be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

inline void eapol_put_be64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * (7 - i))) & 0xff);
}

/* Parse an EAPOL frame (starting at the 802.1X version octet, i.e. what
 * follows the LLC/SNAP header of an 0x888E data frame).
 *
 * Everything is bounds-checked against `len` because this arrives from the
 * air before anything has authenticated it. In particular the declared key
 * data length is checked against what is actually present: a frame claiming
 * 4096 bytes of key data in a 99-byte buffer is the first thing an attacker
 * tries, and the GTK is read out of that region.
 *
 * `len` may be longer than the frame (a padded MSDU); the 802.1X body length
 * is authoritative and is what bounds the parse. A body length longer than
 * the buffer is refused rather than clamped - clamping would let a truncated
 * frame's MIC be computed over fewer bytes than the sender signed.
 */
inline bool parse_eapol_key(const uint8_t* eapol, size_t len, EapolKey* out) {
  size_t body_len, total, kdlen;

  if (!eapol || !out || len < kEapolKeyFixedLen) return false;
  if (eapol[1] != 3) return false;             /* packet type: EAPOL-Key */
  body_len = eapol_be16(eapol + 2);
  /* The body starts at offset 4; the fixed part is 95 bytes of body. */
  if (body_len < kEapolKeyFixedLen - 4) return false;
  total = 4 + body_len;
  if (total > len) return false;

  if (eapol[4] != kKeyDescTypeRsn) return false;
  kdlen = eapol_be16(eapol + kEapolKeyDataLenOff);
  if (kEapolKeyFixedLen + kdlen != total) return false;

  *out = EapolKey{};
  out->frame = eapol;
  out->frame_len = total;
  out->descriptor = eapol[4];
  out->key_info = eapol_be16(eapol + 5);
  out->version = (uint16_t)(out->key_info & kKiVersionMask);
  out->key_len = eapol_be16(eapol + 7);
  out->replay = eapol_be64(eapol + kEapolReplayOff);
  out->nonce = eapol + kEapolNonceOff;
  out->rsc = eapol + kEapolRscOff;
  out->mic = eapol + kEapolMicOff;
  out->key_data = kdlen ? eapol + kEapolKeyFixedLen : nullptr;
  out->key_data_len = kdlen;
  return true;
}

/* Build an EAPOL-Key frame. `mic_kck` non-null sets the MIC over the finished
 * frame with the MIC field zeroed, which is the only order that works: the
 * MIC covers the key data and the key data length, so nothing may be appended
 * afterwards.
 *
 * `proto_version` IS THE 802.1X VERSION OCTET, and it is an argument because
 * it sits inside the MIC'd region and the two reference implementations do
 * not agree on it: hostapd sends 2 and wpa_supplicant sends 1, in the same
 * exchange, and each accepts the other. It is "the highest version the sender
 * supports", not a negotiation, so neither is wrong.
 *
 * The default is 2 because the authenticators in this tree send 2. A
 * supplicant should send kEapolVersionSupplicant - see the note there. */
inline std::vector<uint8_t> build_eapol_key(uint16_t key_info, uint16_t key_len,
                                            uint64_t replay,
                                            const uint8_t nonce[32],
                                            const uint8_t rsc[8],
                                            const uint8_t* key_data,
                                            size_t key_data_len,
                                            CryptoOps* crypto,
                                            const uint8_t* mic_kck,
                                            uint8_t proto_version = 2) {
  std::vector<uint8_t> e(kEapolKeyFixedLen, 0);

  e[0] = proto_version;                        /* 802.1X version */
  e[1] = 3;                                    /* EAPOL-Key */
  const size_t body = kEapolKeyFixedLen - 4 + key_data_len;
  e[2] = (uint8_t)((body >> 8) & 0xff);
  e[3] = (uint8_t)(body & 0xff);
  e[4] = kKeyDescTypeRsn;
  e[5] = (uint8_t)(key_info >> 8);
  e[6] = (uint8_t)(key_info & 0xff);
  e[7] = (uint8_t)(key_len >> 8);
  e[8] = (uint8_t)(key_len & 0xff);
  eapol_put_be64(e.data() + kEapolReplayOff, replay);
  if (nonce) std::memcpy(e.data() + kEapolNonceOff, nonce, 32);
  if (rsc) std::memcpy(e.data() + kEapolRscOff, rsc, 8);
  e[kEapolKeyDataLenOff] = (uint8_t)((key_data_len >> 8) & 0xff);
  e[kEapolKeyDataLenOff + 1] = (uint8_t)(key_data_len & 0xff);
  if (key_data && key_data_len)
    e.insert(e.end(), key_data, key_data + key_data_len);
  if (crypto && mic_kck) {
    uint8_t d[20];

    std::memset(e.data() + kEapolMicOff, 0, kEapolMicLen);
    if (crypto->hmac_sha1(mic_kck, 16, e.data(), e.size(), d))
      std::memcpy(e.data() + kEapolMicOff, d, kEapolMicLen);
    else
      e.clear();                               /* refuse, do not ship unsigned */
  }
  return e;
}

/* Verify an EAPOL-Key MIC with the KCK.
 *
 * THE COMPARISON IS THE POINT. It is done over a copy with the MIC field
 * zeroed - the field is part of the signed region, so it has to be removed
 * before the HMAC, and doing that in place would write through a pointer into
 * a received frame. The result is compared with a constant-time reduction:
 * this runs against attacker-supplied input, and an early-exit memcmp over a
 * MAC is the textbook way to hand out a forgery oracle.
 *
 * A wrong descriptor version fails here rather than being tolerated, because
 * version 1 and 3 use different MIC algorithms entirely.
 */
inline bool eapol_mic_ok(CryptoOps& crypto, const uint8_t kck[16],
                         const EapolKey& k) {
  std::vector<uint8_t> copy(k.frame, k.frame + k.frame_len);
  uint8_t got[kEapolMicLen], want[20];
  uint8_t diff = 0;

  if (k.version != kKeyDescVersionCcmp) return false;
  if (!k.has_mic()) return false;
  if (k.frame_len < kEapolKeyFixedLen) return false;
  std::memcpy(got, k.frame + kEapolMicOff, kEapolMicLen);
  std::memset(copy.data() + kEapolMicOff, 0, kEapolMicLen);
  if (!crypto.hmac_sha1(kck, 16, copy.data(), copy.size(), want)) return false;
  for (size_t i = 0; i < kEapolMicLen; i++) diff |= (uint8_t)(got[i] ^ want[i]);
  return diff == 0;
}

/* The 802.11 PRF built on HMAC-SHA1 (802.11-2016 12.7.1.2). `olen` bytes are
 * produced 20 at a time; the label's terminating NUL is part of the input. */
inline bool prf_sha1(CryptoOps& crypto, const uint8_t* key, size_t key_len,
                     const char* label, const uint8_t* data, size_t data_len,
                     uint8_t* out, size_t olen) {
  const size_t ll = std::strlen(label);
  std::vector<uint8_t> buf(ll + 1 + data_len + 1);

  std::memcpy(buf.data(), label, ll);
  buf[ll] = 0;
  if (data_len) std::memcpy(buf.data() + ll + 1, data, data_len);
  for (size_t gen = 0, i = 0; gen < olen; gen += 20, i++) {
    uint8_t d[20];
    const size_t take = (olen - gen < 20) ? olen - gen : 20;

    buf[ll + 1 + data_len] = (uint8_t)i;
    if (!crypto.hmac_sha1(key, key_len, buf.data(), buf.size(), d))
      return false;
    std::memcpy(out + gen, d, take);
  }
  return true;
}

/* PMK = PBKDF2(passphrase, SSID, 4096, 32). The SSID is the salt, which is
 * why two networks with the same passphrase and different names do not share
 * a PMK. */
inline bool pmk_from_psk(CryptoOps& crypto, const char* passphrase,
                         const std::string& ssid, uint8_t pmk[32]) {
  if (!passphrase || ssid.empty() || ssid.size() > 32) return false;
  return crypto.pbkdf2_sha1(passphrase, (const uint8_t*)ssid.data(),
                            ssid.size(), 4096, pmk, 32);
}

/* PTK = PRF-384(PMK, "Pairwise key expansion",
 *               min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(...))
 *
 * THE SORTING IS NOT DECORATION. Both ends derive the same key only because
 * each orders the pair the same way, and each end knows the addresses and
 * nonces by different names - the authenticator's "own" is the supplicant's
 * "peer". Sorting removes the asymmetry. Getting it wrong produces a PTK that
 * works against nothing, and the only symptom is a MIC failure.
 *
 * Layout: KCK[0:16] KEK[16:32] TK[32:48].
 */
inline bool derive_ptk(CryptoOps& crypto, const uint8_t pmk[32],
                       const uint8_t aa[6], const uint8_t spa[6],
                       const uint8_t anonce[32], const uint8_t snonce[32],
                       uint8_t ptk[48]) {
  uint8_t b[76];
  const uint8_t* amin = std::memcmp(aa, spa, 6) < 0 ? aa : spa;
  const uint8_t* amax = std::memcmp(aa, spa, 6) < 0 ? spa : aa;
  const uint8_t* nmin = std::memcmp(anonce, snonce, 32) < 0 ? anonce : snonce;
  const uint8_t* nmax = std::memcmp(anonce, snonce, 32) < 0 ? snonce : anonce;

  std::memcpy(b, amin, 6);
  std::memcpy(b + 6, amax, 6);
  std::memcpy(b + 12, nmin, 32);
  std::memcpy(b + 44, nmax, 32);
  return prf_sha1(crypto, pmk, 32, "Pairwise key expansion", b, sizeof b, ptk,
                  48);
}

/* A GTK lifted out of a key-data KDE. */
struct GtkKde {
  uint8_t key_id = 0;
  bool tx = false;
  uint8_t gtk[32] = {0};
  size_t gtk_len = 0;
};

/* THREE OUTCOMES, NOT TWO. "there is no GTK KDE here" and "a KDE in here is
 * truncated or claims an impossible key length" are completely different
 * facts: the first can be legitimate, the second is hostile input. They were
 * one `false` return until 2026-09-21, and the two callers read that same
 * false in opposite ways - one carried on and completed the handshake, the
 * other refused the frame. A station left `Connected` and keyed with no group
 * key, no counter moved and nothing to diagnose from. */
enum class KdeResult : uint8_t {
  Found,
  Absent,     /* well-formed key data with no GTK KDE in it */
  Malformed,  /* a KDE that runs past the buffer or declares a bad length */
};

/* Walk the key-data field for the GTK KDE (00-0F-AC type 1).
 *
 * The key data is a sequence of elements: a KDE is `0xDD len 00 0F AC type`
 * followed by its body, and anything else (an RSN element, the 802.11i
 * padding of 0xDD followed by zeros) is skipped. Every step is bounds-checked
 * - this region comes out of an AES unwrap of attacker-supplied bytes, and a
 * failed unwrap that was not checked would hand this loop pure noise.
 *
 * Returns Absent when the key data is well formed and simply carries no GTK
 * KDE, and Malformed when something in it does not add up. The caller decides
 * what Absent means for the message it arrived in.
 */
inline KdeResult find_gtk_kde(const uint8_t* kd, size_t len, GtkKde* out) {
  size_t i = 0;

  if (!kd || !out) return KdeResult::Malformed;
  while (i + 2 <= len) {
    const uint8_t eid = kd[i];
    const size_t elen = kd[i + 1];

    if (eid == 0x00) { i++; continue; }        /* padding */
    if (i + 2 + elen > len)
      return KdeResult::Malformed;             /* truncated: stop, do not guess */
    if (eid == 0xdd && elen >= 4 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0f &&
        kd[i + 4] == 0xac && kd[i + 5] == 0x01) {
      /* The KDE length counts OUI(3) + data type(1) + data. The GTK KDE's
       * data is a KeyID/Tx octet, a reserved octet, then the key, so a
       * 16-byte GTK gives a length of 22 - and subtracting 6 rather than 4
       * here, which an earlier draft did, makes every real KDE look two bytes
       * short and silently truncates the key. */
      const size_t body = elen - 4;            /* keyid/tx octet + reserved + key */

      if (body < 2 + 16 || body > 2 + 32) return KdeResult::Malformed;
      *out = GtkKde{};
      out->key_id = (uint8_t)(kd[i + 6] & 0x03);
      out->tx = (kd[i + 6] & 0x04) != 0;
      out->gtk_len = body - 2;
      std::memcpy(out->gtk, kd + i + 8, out->gtk_len);
      return KdeResult::Found;
    }
    i += 2 + elen;
  }
  return KdeResult::Absent;
}

/* Overwrite key material so it does not outlive the object holding it.
 *
 * Through a volatile pointer, because a compiler is entitled to delete a
 * memset whose result is never read - which is every memset in a destructor.
 * docs/station-mode-scope.md lists "keys zeroized on teardown" among the
 * PR #335 review items, and src/sta/StationTable.h has done it for the AP
 * side since Phase 2b; this is the station half of the same rule.
 */
inline void secure_wipe(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);

  while (n--) *v++ = 0;
}

}  // namespace sta
}  // namespace devourer

#endif /* DEVOURER_STA_EAPOL_H */
