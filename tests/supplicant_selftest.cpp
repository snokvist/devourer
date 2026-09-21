/* Headless guard for src/sta/Eapol.h and src/sta/Supplicant.h — the station
 * half of the WPA2-PSK key exchange.
 *
 * PHASE 3'S ACCEPTANCE IS TWO NEGATIVE CASES, and they are named here so that
 * a future edit that deletes them has to delete a stated requirement:
 *
 *   test_forged_mic_is_rejected()        — a forged EAPOL-Key MIC
 *   test_group_rekey_replay_rejected()   — an equal-counter group rekey
 *
 * Both shipped as defects in PR #335, in a reviewed pull request. A phase that
 * cannot fail these two tests has not been tested.
 *
 * WHAT IS AND IS NOT INDEPENDENT HERE. The authenticator below is a fixture,
 * and it builds its frames with the same `build_eapol_key` the supplicant
 * parses — so this file pins the STATE MACHINE and the CHECKS, not the wire
 * format. Two things cover the format instead: the known-answer vectors at
 * the top, which come from IEEE 802.11i Annex H.4 and were cross-checked
 * against Python's hashlib (a third implementation), and the cross-role cell
 * in tests/ap_wpa2_selftest.inc, where this supplicant talks to an
 * authenticator that hand-rolls every offset independently.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "openssl_crypto_ops.h"
#include "sta/Eapol.h"
#include "sta/Supplicant.h"

namespace {

using devourer::sta::EapolKey;
using devourer::sta::Supplicant;
using devourer::test::OpenSslCryptoOps;

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_fail++;
  }
}

const uint8_t kAa[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x00};   /* the AP */
const uint8_t kSpa[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};  /* the station */
const char* kPsk = "devourer123";
const char* kSsid = "devourerAP";

/* ---- known answers ------------------------------------------------------
 *
 * PR #335 shipped hand-rolled PBKDF2 and the 802.11 PRF with no known-answer
 * test at all, and its review named that the single highest-leverage gap in
 * the PR. These are the IEEE 802.11i Annex H.4.2 passphrase-to-PSK vectors —
 * published, widely reproduced, and recomputed with Python's hashlib before
 * being written down here, so they are not this repository's own arithmetic
 * repeated back at itself. */
struct PskVector {
  const char* passphrase;
  const char* ssid;
  const char* pmk_hex;
};
const PskVector kPskVectors[] = {
    {"password", "IEEE",
     "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"},
    {"ThisIsAPassword", "ThisIsASSID",
     "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"},
    {"aaaaaaaaaa", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ",
     "727c76be68d32158bed0787c002aaf97ac38a34d11da8be1afdcf3e96965346d"},
};

std::vector<uint8_t> unhex(const char* h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
    auto nib = [](char c) -> int {
      return c >= 'a' ? c - 'a' + 10 : (c >= 'A' ? c - 'A' + 10 : c - '0');
    };
    v.push_back((uint8_t)((nib(h[i]) << 4) | nib(h[i + 1])));
  }
  return v;
}

void test_psk_known_answers() {
  OpenSslCryptoOps c;

  for (const PskVector& v : kPskVectors) {
    uint8_t pmk[32];
    const std::vector<uint8_t> want = unhex(v.pmk_hex);
    char label[96];

    std::snprintf(label, sizeof label, "PSK vector '%s' / '%s'", v.passphrase,
                  v.ssid);
    check(devourer::sta::pmk_from_psk(c, v.passphrase, v.ssid, pmk), label);
    check(want.size() == 32 && std::memcmp(pmk, want.data(), 32) == 0, label);
  }

  /* The SSID is the SALT, which is why the same passphrase on two networks
   * does not share a PMK. Without this, dropping the salt entirely would pass
   * every symmetry test in the file. */
  uint8_t a[32], b[32];
  devourer::sta::pmk_from_psk(c, "password", "IEEE", a);
  devourer::sta::pmk_from_psk(c, "password", "IEEF", b);
  check(std::memcmp(a, b, 32) != 0, "the SSID is part of the derivation");

  /* Refusals: an SSID must be 1..32 octets. */
  check(!devourer::sta::pmk_from_psk(c, "password", "", a),
        "an empty SSID is refused");
  check(!devourer::sta::pmk_from_psk(c, "password", std::string(33, 'x'), a),
        "a 33-octet SSID is refused");
}

/* The PTK derivation's address and nonce sorting, asserted directly. Both
 * ends derive the same key ONLY because each sorts the pair the same way, and
 * each end knows them by opposite names. */
void test_ptk_sorting() {
  OpenSslCryptoOps c;
  uint8_t pmk[32], n1[32], n2[32], p1[48], p2[48], p3[48];

  std::memset(pmk, 0x11, 32);
  std::memset(n1, 0xa0, 32);
  std::memset(n2, 0xb0, 32);

  check(devourer::sta::derive_ptk(c, pmk, kAa, kSpa, n1, n2, p1), "derive");
  check(devourer::sta::derive_ptk(c, pmk, kSpa, kAa, n1, n2, p2), "derive");
  check(std::memcmp(p1, p2, 48) == 0,
        "swapping the two addresses derives the SAME PTK");
  check(devourer::sta::derive_ptk(c, pmk, kAa, kSpa, n2, n1, p3), "derive");
  check(std::memcmp(p1, p3, 48) == 0,
        "swapping the two nonces derives the SAME PTK");

  /* And it is not simply constant: a different nonce is a different key. */
  uint8_t n3[32], p4[48];
  std::memset(n3, 0xc0, 32);
  devourer::sta::derive_ptk(c, pmk, kAa, kSpa, n1, n3, p4);
  check(std::memcmp(p1, p4, 48) != 0, "a different nonce is a different PTK");

  /* The three sections must not alias: KCK, KEK and TK are different bytes.
   * A PRF that produced 48 identical bytes would pass every test above. */
  check(std::memcmp(p1, p1 + 16, 16) != 0 &&
            std::memcmp(p1 + 16, p1 + 32, 16) != 0,
        "KCK, KEK and TK differ");
}

/* ---- the fixture authenticator -----------------------------------------
 *
 * Enough of an authenticator to drive a supplicant through everything it
 * implements, and no more: no retransmission schedule, no station table.
 */
struct Authenticator {
  OpenSslCryptoOps crypto;
  uint8_t pmk[32] = {0};
  uint8_t anonce[32] = {0};
  uint8_t ptk[48] = {0};
  uint8_t gtk[16] = {0};
  uint8_t gtk_keyid = 1;
  uint64_t replay = 0;
  bool have_ptk = false;

  void init() {
    devourer::sta::pmk_from_psk(crypto, kPsk, kSsid, pmk);
    std::memset(anonce, 0x5e, 32);
    std::memset(gtk, 0x31, 16);
  }

  std::vector<uint8_t> msg1() {
    replay++;
    return devourer::sta::build_eapol_key(
        devourer::sta::kKeyDescVersionCcmp | devourer::sta::kKiPairwise |
            devourer::sta::kKiAck,
        16, replay, anonce, nullptr, nullptr, 0, nullptr, nullptr);
  }

  /* Derive from the supplicant's SNonce, then verify its MIC. */
  bool on_msg2(const std::vector<uint8_t>& e) {
    EapolKey k;

    if (!devourer::sta::parse_eapol_key(e.data(), e.size(), &k)) return false;
    if (!devourer::sta::derive_ptk(crypto, pmk, kAa, kSpa, anonce, k.nonce,
                                   ptk))
      return false;
    if (!devourer::sta::eapol_mic_ok(crypto, ptk, k)) return false;
    have_ptk = true;
    return true;
  }

  /* Key data: the RSN element then a GTK KDE, 802.11i-padded, AES-wrapped. */
  std::vector<uint8_t> wrapped_gtk(const uint8_t* key, uint8_t keyid) {
    std::vector<uint8_t> kd;
    devourer::sta::append_rsn_ccmp_psk(kd);
    const uint8_t hdr[8] = {0xdd, 0x16, 0x00, 0x0f, 0xac, 0x01, keyid, 0x00};
    kd.insert(kd.end(), hdr, hdr + 8);
    kd.insert(kd.end(), key, key + 16);
    if (kd.size() % 8) {
      kd.push_back(0xdd);
      while (kd.size() % 8) kd.push_back(0x00);
    }
    std::vector<uint8_t> w(kd.size() + 8);
    const int n = OpenSslCryptoOps::key_wrap(ptk + 16, 16, kd.data(),
                                             kd.size(), w.data());
    w.resize(n > 0 ? (size_t)n : 0);
    return w;
  }

  std::vector<uint8_t> msg3() {
    const std::vector<uint8_t> w = wrapped_gtk(gtk, gtk_keyid);

    replay++;
    return devourer::sta::build_eapol_key(
        devourer::sta::kKeyDescVersionCcmp | devourer::sta::kKiPairwise |
            devourer::sta::kKiInstall | devourer::sta::kKiAck |
            devourer::sta::kKiMic | devourer::sta::kKiSecure |
            devourer::sta::kKiEncrypted,
        16, replay, anonce, nullptr, w.data(), w.size(), &crypto, ptk);
  }

  bool on_msg4(const std::vector<uint8_t>& e) {
    EapolKey k;

    if (!devourer::sta::parse_eapol_key(e.data(), e.size(), &k)) return false;
    return devourer::sta::eapol_mic_ok(crypto, ptk, k);
  }

  /* Group key handshake message 1, at a chosen replay counter so a test can
   * hand the supplicant one it has already seen. */
  std::vector<uint8_t> group1(const uint8_t* key, uint8_t keyid,
                              uint64_t at_replay) {
    const std::vector<uint8_t> w = wrapped_gtk(key, keyid);

    return devourer::sta::build_eapol_key(
        devourer::sta::kKeyDescVersionCcmp | devourer::sta::kKiAck |
            devourer::sta::kKiMic | devourer::sta::kKiSecure |
            devourer::sta::kKiEncrypted,
        16, at_replay, nullptr, nullptr, w.data(), w.size(), &crypto, ptk);
  }

  std::vector<uint8_t> group1_next(const uint8_t* key, uint8_t keyid) {
    replay++;
    return group1(key, keyid, replay);
  }
};

/* Run a complete four-way. Leaves both sides keyed. */
bool handshake(Authenticator& ap, Supplicant& sup, OpenSslCryptoOps& crypto) {
  uint8_t snonce[32];
  std::vector<uint8_t> out;

  std::memset(snonce, 0x7a, 32);
  ap.init();
  sup.start(crypto, ap.pmk, kSpa, kAa, snonce);

  const std::vector<uint8_t> m1 = ap.msg1();
  if (sup.on_eapol(m1.data(), m1.size(), &out) != Supplicant::Verdict::Reply)
    return false;
  if (!ap.on_msg2(out)) return false;

  const std::vector<uint8_t> m3 = ap.msg3();
  if (sup.on_eapol(m3.data(), m3.size(), &out) != Supplicant::Verdict::Reply)
    return false;
  return ap.on_msg4(out);
}

/* ---- the four-way ------------------------------------------------------ */

void test_four_way() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;

  check(handshake(ap, sup, crypto), "the four-way completes");
  check(sup.state() == Supplicant::State::Done, "the supplicant is Done");
  check(sup.ptk_valid(), "the PTK is installed");
  check(std::memcmp(sup.ptk(), ap.ptk, 48) == 0,
        "both sides derived the SAME PTK");
  check(sup.gtk_valid() && sup.gtk_len() == 16, "the GTK is installed");
  check(std::memcmp(sup.gtk(), ap.gtk, 16) == 0,
        "the GTK is the one the authenticator sent");
  check(sup.gtk_key_id() == ap.gtk_keyid,
        "the GTK's key id survives the KDE — a group frame at the wrong key id "
        "is looked up as the pairwise key and MIC-fails");
  check(sup.mic_failures == 0 && sup.replays == 0 && sup.malformed == 0,
        "a clean handshake trips no refusal counter");
}

/* ACCEPTANCE NEGATIVE 1. Message 3 carries the GTK and confirms the PTK. A
 * forged MIC must change nothing: not the state, not the installed keys, not
 * the replay counter, and it must produce no message 4 — replying would tell
 * an attacker the forgery was accepted even if the keys were not installed. */
void test_forged_mic_is_rejected() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  uint8_t snonce[32];
  std::vector<uint8_t> out;

  std::memset(snonce, 0x7a, 32);
  ap.init();
  sup.start(crypto, ap.pmk, kSpa, kAa, snonce);

  const std::vector<uint8_t> m1 = ap.msg1();
  check(sup.on_eapol(m1.data(), m1.size(), &out) == Supplicant::Verdict::Reply,
        "msg1 is answered");
  check(ap.on_msg2(out), "the authenticator accepts msg2");
  check(!sup.ptk_valid(), "nothing is installed before msg3");

  std::vector<uint8_t> m3 = ap.msg3();
  const uint64_t before_replay = sup.replay_counter();

  /* One bit, in the MIC field. Everything else about this frame is correct —
   * the counter advances, the ANonce matches, the key data unwraps. */
  m3[devourer::sta::kEapolMicOff] ^= 0x01;
  out.clear();
  check(sup.on_eapol(m3.data(), m3.size(), &out) ==
            Supplicant::Verdict::MicFailed,
        "A FORGED EAPOL-Key MIC IS REJECTED");
  check(sup.mic_failures == 1, "...and counted");
  check(out.empty(), "...with no message 4 sent");
  check(!sup.ptk_valid(), "...the PTK is NOT installed");
  check(!sup.gtk_valid(), "...the GTK is NOT installed");
  check(sup.state() == Supplicant::State::WaitMsg3,
        "...and the state does not advance");
  check(sup.replay_counter() == before_replay,
        "...and the replay counter does not advance, so the real msg3 still "
        "works");

  /* THE POSITIVE ARM. Without it, a supplicant that rejected everything would
   * pass the test above. The genuine message 3 must still be accepted. */
  const std::vector<uint8_t> good = ap.msg3();
  out.clear();
  check(sup.on_eapol(good.data(), good.size(), &out) ==
            Supplicant::Verdict::Reply,
        "the genuine msg3 is still accepted afterwards");
  check(sup.ptk_valid() && sup.gtk_valid(), "...and installs both keys");
  check(ap.on_msg4(out), "...and its msg4 verifies at the authenticator");
}

/* ACCEPTANCE NEGATIVE 2. Replaying a captured group message 1 must not
 * reinstall its GTK. The old key comes back with its PN space reset, so every
 * group frame sent since is encrypted again under a nonce already used — CCM
 * keystream reuse across the whole BSS, from one captured frame. */
void test_group_rekey_replay_rejected() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  std::vector<uint8_t> out;
  uint8_t gtk2[16], gtk3[16];

  check(handshake(ap, sup, crypto), "the four-way completes");
  std::memset(gtk2, 0x62, 16);
  std::memset(gtk3, 0x93, 16);

  /* A genuine rekey first, so the test is not measuring a supplicant that
   * refuses all group messages. */
  const std::vector<uint8_t> g1 = ap.group1_next(gtk2, 2);
  check(sup.on_eapol(g1.data(), g1.size(), &out) == Supplicant::Verdict::Reply,
        "a genuine group rekey is accepted");
  check(sup.gtk_valid() && std::memcmp(sup.gtk(), gtk2, 16) == 0,
        "...and installs the new GTK");
  check(sup.gtk_key_id() == 2, "...at its key id");
  const uint64_t after = sup.replay_counter();

  /* THE REPLAY. Byte-identical to the frame just accepted, which is what an
   * attacker captures off the air. Its MIC is valid — that is the point. */
  out.clear();
  const Supplicant::Verdict v = sup.on_eapol(g1.data(), g1.size(), &out);
  check(v == Supplicant::Verdict::Retransmit,
        "AN EQUAL-COUNTER GROUP REKEY IS NOT TREATED AS NEW");
  check(std::memcmp(sup.gtk(), gtk2, 16) == 0,
        "...the GTK is unchanged (it was already this one)");
  check(sup.replay_counter() == after, "...the counter does not move");

  /* The case that actually matters: the SAME counter carrying a DIFFERENT
   * key. This is a captured older rekey, and installing it is the defect —
   * the supplicant must not take the key out of it. */
  const std::vector<uint8_t> forged = ap.group1(gtk3, 3, after);
  out.clear();
  check(sup.on_eapol(forged.data(), forged.size(), &out) ==
            Supplicant::Verdict::Retransmit,
        "AN EQUAL-COUNTER REKEY CARRYING A DIFFERENT GTK IS REFUSED");
  check(std::memcmp(sup.gtk(), gtk2, 16) == 0,
        "...AND THE OLD GTK IS NOT REINSTALLED");
  check(sup.gtk_key_id() == 2, "...nor its key id");

  /* A LOWER counter is a plain replay and is refused outright. */
  const std::vector<uint8_t> older = ap.group1(gtk3, 3, after - 1);
  out.clear();
  check(sup.on_eapol(older.data(), older.size(), &out) ==
            Supplicant::Verdict::Replayed,
        "a LOWER counter is refused outright");
  check(sup.replays == 1, "...and counted");
  check(std::memcmp(sup.gtk(), gtk2, 16) == 0, "...installing nothing");

  /* POSITIVE ARM: a strictly greater counter still works, so the rule above
   * is a window and not a wall. */
  const std::vector<uint8_t> g2 = ap.group1_next(gtk3, 3);
  out.clear();
  check(sup.on_eapol(g2.data(), g2.size(), &out) == Supplicant::Verdict::Reply,
        "a greater counter installs the new GTK");
  check(std::memcmp(sup.gtk(), gtk3, 16) == 0, "...and it is the new key");
}

/* A group message whose MIC does not verify installs nothing either — the
 * same rule as message 3, on the path that runs for the life of the
 * association rather than once at the start. */
void test_group_forged_mic_rejected() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  std::vector<uint8_t> out;
  uint8_t gtk2[16];

  check(handshake(ap, sup, crypto), "the four-way completes");
  std::memset(gtk2, 0x62, 16);

  std::vector<uint8_t> g1 = ap.group1_next(gtk2, 2);
  g1[devourer::sta::kEapolMicOff + 7] ^= 0x80;
  check(sup.on_eapol(g1.data(), g1.size(), &out) ==
            Supplicant::Verdict::MicFailed,
        "a forged group-rekey MIC is rejected");
  check(std::memcmp(sup.gtk(), ap.gtk, 16) == 0,
        "...and the GTK from the four-way is still installed");
}

/* A group rekey arriving BEFORE the four-way has finished has no key to be
 * verified with. Removing the state guard survived this file until this cell
 * existed, because nothing ever sent one early — and what it would have left
 * behind is a MIC check run against an all-zero PTK, which is a comparison of
 * attacker-supplied bytes against a key the station does not have. */
void test_group_rekey_before_four_way() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  uint8_t snonce[32], gtk2[16];
  std::vector<uint8_t> out;

  std::memset(snonce, 0x7a, 32);
  std::memset(gtk2, 0x62, 16);
  ap.init();
  sup.start(crypto, ap.pmk, kSpa, kAa, snonce);

  /* ap.ptk is still zero here, which is exactly the situation: neither side
   * has a pairwise key yet. */
  const std::vector<uint8_t> g = ap.group1(gtk2, 2, 1);
  check(sup.on_eapol(g.data(), g.size(), &out) ==
            Supplicant::Verdict::OutOfState,
        "a group rekey before the four-way is out of state");
  check(sup.out_of_state == 1, "...and counted as such, not as a MIC failure");
  check(!sup.gtk_valid(), "...installing nothing");

  /* Same again after msg1 but before msg3: a PTK is a CANDIDATE at that
   * point, not an installed key, and a group rekey must not use it. */
  const std::vector<uint8_t> m1 = ap.msg1();
  sup.on_eapol(m1.data(), m1.size(), &out);
  check(sup.state() == Supplicant::State::WaitMsg3, "msg1 was accepted");
  const std::vector<uint8_t> g2 = ap.group1(gtk2, 2, 5);
  out.clear();
  check(sup.on_eapol(g2.data(), g2.size(), &out) ==
            Supplicant::Verdict::OutOfState,
        "a group rekey between msg1 and msg3 is out of state too");
  check(!sup.gtk_valid(), "...still installing nothing");
}

/* eapol_mic_ok() refuses a descriptor version whose MIC is a different
 * algorithm, and that refusal has to be asserted HERE rather than through the
 * supplicant: on_eapol() rejects a wrong version before the MIC is ever
 * reached, so a mutation deleting the check inside eapol_mic_ok survived the
 * whole file. The function is public and an authenticator calls it directly. */
void test_mic_refuses_other_descriptor_versions() {
  OpenSslCryptoOps crypto;
  uint8_t kck[16];
  EapolKey k;

  std::memset(kck, 0x2b, 16);
  for (uint16_t ver : {1, 2, 3}) {
    /* build_eapol_key computes HMAC-SHA1 whatever the version says, so each
     * of these frames carries a MIC that is correct FOR THAT ALGORITHM. Only
     * version 2 declares that algorithm. */
    const std::vector<uint8_t> e = devourer::sta::build_eapol_key(
        (uint16_t)(ver | devourer::sta::kKiPairwise | devourer::sta::kKiMic),
        16, 1, nullptr, nullptr, nullptr, 0, &crypto, kck);
    char label[96];

    check(devourer::sta::parse_eapol_key(e.data(), e.size(), &k), "parses");
    std::snprintf(label, sizeof label,
                  "eapol_mic_ok %s descriptor version %u",
                  ver == 2 ? "accepts" : "refuses", ver);
    check(devourer::sta::eapol_mic_ok(crypto, kck, k) == (ver == 2), label);
  }

  /* And the positive arm is not an accident: the wrong KCK on a version-2
   * frame still fails. */
  const std::vector<uint8_t> e = devourer::sta::build_eapol_key(
      devourer::sta::kKeyDescVersionCcmp | devourer::sta::kKiPairwise |
          devourer::sta::kKiMic,
      16, 1, nullptr, nullptr, nullptr, 0, &crypto, kck);
  devourer::sta::parse_eapol_key(e.data(), e.size(), &k);
  uint8_t other[16];
  std::memset(other, 0x99, 16);
  check(!devourer::sta::eapol_mic_ok(crypto, other, k),
        "a version-2 MIC under the wrong KCK is refused");
}

/* 12.7.6.4: message 3's ANonce must equal message 1's. A mismatch is a
 * mix-and-match of two exchanges and is refused BEFORE the MIC, so it reads
 * as what it is rather than as a key failure. */
void test_anonce_mismatch_rejected() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  uint8_t snonce[32];
  std::vector<uint8_t> out;

  std::memset(snonce, 0x7a, 32);
  ap.init();
  sup.start(crypto, ap.pmk, kSpa, kAa, snonce);
  const std::vector<uint8_t> m1 = ap.msg1();
  sup.on_eapol(m1.data(), m1.size(), &out);
  ap.on_msg2(out);

  std::vector<uint8_t> m3 = ap.msg3();
  m3[devourer::sta::kEapolNonceOff] ^= 0xff;
  out.clear();
  check(sup.on_eapol(m3.data(), m3.size(), &out) ==
            Supplicant::Verdict::Malformed,
        "msg3 with a different ANonce is refused");
  check(!sup.ptk_valid(), "...and installs nothing");
}

/* Message 3 with its key data in the CLEAR is a downgrade: the GTK would be
 * readable off the air. Refused rather than parsed. */
void test_unencrypted_key_data_rejected() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  uint8_t snonce[32];
  std::vector<uint8_t> out;

  std::memset(snonce, 0x7a, 32);
  ap.init();
  sup.start(crypto, ap.pmk, kSpa, kAa, snonce);
  const std::vector<uint8_t> m1 = ap.msg1();
  sup.on_eapol(m1.data(), m1.size(), &out);
  ap.on_msg2(out);

  /* Build msg3 with the GTK KDE unwrapped and the Encrypted bit clear, then
   * MIC it correctly — so the ONLY thing wrong is the missing encryption. */
  std::vector<uint8_t> kd;
  const uint8_t hdr[8] = {0xdd, 0x16, 0x00, 0x0f, 0xac, 0x01, 1, 0x00};
  kd.insert(kd.end(), hdr, hdr + 8);
  kd.insert(kd.end(), ap.gtk, ap.gtk + 16);
  const std::vector<uint8_t> m3 = devourer::sta::build_eapol_key(
      devourer::sta::kKeyDescVersionCcmp | devourer::sta::kKiPairwise |
          devourer::sta::kKiInstall | devourer::sta::kKiAck |
          devourer::sta::kKiMic | devourer::sta::kKiSecure,
      16, ap.replay + 1, ap.anonce, nullptr, kd.data(), kd.size(), &ap.crypto,
      ap.ptk);
  out.clear();
  check(sup.on_eapol(m3.data(), m3.size(), &out) ==
            Supplicant::Verdict::Malformed,
        "msg3 with unencrypted key data is refused");
  check(!sup.gtk_valid(), "...and the GTK is not taken from the clear");
}

/* The descriptor version selects the MIC ALGORITHM. Version 1 is HMAC-MD5 and
 * version 3 is AES-CMAC; treating either as version 2 means verifying with
 * the wrong primitive, and the only symptom would be a MIC failure pointing
 * at the key. */
void test_descriptor_version_refused() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  uint8_t snonce[32];
  std::vector<uint8_t> out;

  std::memset(snonce, 0x7a, 32);
  ap.init();
  sup.start(crypto, ap.pmk, kSpa, kAa, snonce);

  for (uint8_t ver : {1, 3}) {
    std::vector<uint8_t> m1 = ap.msg1();
    char label[80];

    m1[6] = (uint8_t)((m1[6] & ~0x07) | ver);
    std::snprintf(label, sizeof label, "key descriptor version %u is refused",
                  ver);
    out.clear();
    check(sup.on_eapol(m1.data(), m1.size(), &out) ==
              Supplicant::Verdict::Malformed,
          label);
  }
  check(sup.state() == Supplicant::State::WaitMsg1,
        "...and none of them started a handshake");
}

/* The replay rule on the PAIRWISE path, including the retransmission window
 * that makes it a rule rather than a wall. */
void test_replay_counter_rules() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  uint8_t snonce[32];
  std::vector<uint8_t> out, first;

  std::memset(snonce, 0x7a, 32);
  ap.init();
  sup.start(crypto, ap.pmk, kSpa, kAa, snonce);

  const std::vector<uint8_t> m1 = ap.msg1();
  check(sup.on_eapol(m1.data(), m1.size(), &first) ==
            Supplicant::Verdict::Reply,
        "msg1 is answered");

  /* An authenticator that does NOT increment on retransmission (802.11-2016
   * 12.7.6.4 permits either) must still be able to finish. The same reply
   * goes back out, byte for byte, and nothing is re-derived. */
  out.clear();
  check(sup.on_eapol(m1.data(), m1.size(), &out) ==
            Supplicant::Verdict::Retransmit,
        "an equal counter is answered as a retransmission");
  check(out == first, "...with the identical reply");
  check(sup.retransmits == 1, "...and counted as one");

  /* A lower counter is refused outright. */
  std::vector<uint8_t> older = ap.msg1();
  devourer::sta::eapol_put_be64(older.data() + devourer::sta::kEapolReplayOff,
                                0);
  out.clear();
  check(sup.on_eapol(older.data(), older.size(), &out) ==
            Supplicant::Verdict::Replayed,
        "a lower counter is refused");
  check(out.empty(), "...with no reply at all");
}

/* Message 1 arriving on a working association must not disturb it. It is
 * unauthenticated — anyone can send one — so a station that re-derived over
 * its live PTK would lose the link to a single forged frame. */
void test_msg1_on_a_live_association() {
  Authenticator ap;
  Supplicant sup;
  OpenSslCryptoOps crypto;
  std::vector<uint8_t> out;
  uint8_t live[48], gtk_before[16];

  check(handshake(ap, sup, crypto), "the four-way completes");
  std::memcpy(live, sup.ptk(), 48);
  std::memcpy(gtk_before, sup.gtk(), 16);

  /* A forged msg1 with a fresh ANonce and an advancing counter: everything an
   * attacker can produce without the PMK. */
  std::vector<uint8_t> m1 = ap.msg1();
  m1[devourer::sta::kEapolNonceOff] ^= 0xff;
  out.clear();
  check(sup.on_eapol(m1.data(), m1.size(), &out) == Supplicant::Verdict::Reply,
        "msg1 on a live association is answered");
  check(std::memcmp(sup.ptk(), live, 48) == 0,
        "...and the INSTALLED PTK is untouched");
  check(std::memcmp(sup.gtk(), gtk_before, 16) == 0,
        "...as is the GTK");
  check(sup.ptk_valid(), "...and the station stays keyed");
}

/* ---- the wire format, from the air ------------------------------------- */

void test_parse_bounds() {
  Authenticator ap;
  EapolKey k;

  ap.init();
  const std::vector<uint8_t> good = ap.msg1();

  check(devourer::sta::parse_eapol_key(good.data(), good.size(), &k),
        "a well-formed EAPOL-Key parses");
  check(k.frame_len == good.size(), "...to its whole length");
  check(k.key_data_len == 0, "...with no key data");

  for (size_t n = 0; n < good.size(); n++)
    if (devourer::sta::parse_eapol_key(good.data(), n, &k)) {
      check(false, "a truncated frame must not parse");
      break;
    }

  /* A KEY DATA LENGTH THAT LIES. This is the first thing an attacker tries,
   * because the GTK is read out of that region: claim 4096 bytes in a 99-byte
   * frame and a parser that trusts the field reads four kilobytes of the
   * heap. */
  std::vector<uint8_t> liar = good;
  liar[devourer::sta::kEapolKeyDataLenOff] = 0x10;
  liar[devourer::sta::kEapolKeyDataLenOff + 1] = 0x00;
  check(!devourer::sta::parse_eapol_key(liar.data(), liar.size(), &k),
        "a key data length longer than the frame is refused");

  /* A body length longer than the buffer is refused rather than clamped:
   * clamping would compute the MIC over fewer bytes than the sender signed. */
  std::vector<uint8_t> big = good;
  big[2] = 0x7f;
  check(!devourer::sta::parse_eapol_key(big.data(), big.size(), &k),
        "a body length longer than the buffer is refused");

  std::vector<uint8_t> wrong = good;
  wrong[1] = 0;
  check(!devourer::sta::parse_eapol_key(wrong.data(), wrong.size(), &k),
        "a non-EAPOL-Key packet type is refused");
  wrong = good;
  wrong[4] = 1;
  check(!devourer::sta::parse_eapol_key(wrong.data(), wrong.size(), &k),
        "a non-RSN key descriptor type is refused");
}

/* The KDE walker, against the shapes that come out of a failed unwrap. */
void test_gtk_kde() {
  devourer::sta::GtkKde g;
  uint8_t kd[64];

  /* The exact shape a real authenticator emits: length 22 counts the OUI, the
   * data type, and 18 bytes of data. Subtracting 6 rather than 4 here makes
   * every real KDE look short — a draft of this walker did exactly that. */
  const uint8_t good[24] = {0xdd, 0x16, 0x00, 0x0f, 0xac, 0x01, 0x02, 0x00,
                            1, 2, 3, 4, 5, 6, 7, 8,
                            9, 10, 11, 12, 13, 14, 15, 16};
  check(devourer::sta::find_gtk_kde(good, sizeof good, &g), "a GTK KDE parses");
  check(g.gtk_len == 16, "...with a 16-byte key");
  check(g.key_id == 2, "...and its key id");
  check(g.gtk[0] == 1 && g.gtk[15] == 16, "...and the key bytes");

  /* Truncated: the length runs past the buffer. Stop, do not read on. */
  check(!devourer::sta::find_gtk_kde(good, 12, &g),
        "a KDE truncated by the buffer is refused");

  /* A vendor OUI is not ours and must not be mistaken for a GTK. */
  std::memcpy(kd, good, sizeof good);
  kd[2] = 0x00; kd[3] = 0x50; kd[4] = 0xf2;
  check(!devourer::sta::find_gtk_kde(kd, sizeof good, &g),
        "a vendor OUI is not read as a GTK KDE");

  /* A declared length too small to hold a key. */
  std::memcpy(kd, good, sizeof good);
  kd[1] = 0x08;
  check(!devourer::sta::find_gtk_kde(kd, sizeof good, &g),
        "a KDE too short for a key is refused");

  /* And no KDE at all is a clean false, not a crash: an RSN element followed
   * by 802.11i padding is exactly what message 3 carries when there is no
   * group key. */
  std::vector<uint8_t> rsn;
  devourer::sta::append_rsn_ccmp_psk(rsn);
  rsn.push_back(0xdd);
  while (rsn.size() % 8) rsn.push_back(0x00);
  check(!devourer::sta::find_gtk_kde(rsn.data(), rsn.size(), &g),
        "key data with no GTK KDE returns false rather than reading on");
}

}  // namespace

int main() {
  test_psk_known_answers();
  test_ptk_sorting();
  test_parse_bounds();
  test_gtk_kde();
  test_four_way();
  test_forged_mic_is_rejected();
  test_group_rekey_replay_rejected();
  test_group_forged_mic_rejected();
  test_anonce_mismatch_rejected();
  test_unencrypted_key_data_rejected();
  test_descriptor_version_refused();
  test_mic_refuses_other_descriptor_versions();
  test_group_rekey_before_four_way();
  test_replay_counter_rules();
  test_msg1_on_a_live_association();

  if (g_fail) {
    std::printf("supplicant_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("supplicant_selftest: OK\n");
  return 0;
}
