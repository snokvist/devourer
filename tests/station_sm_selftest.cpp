/* Headless guard for src/sta/StationSm.h — the association state machine.
 *
 * Authenticate, associate, four-way, connected; and every way that stops.
 *
 * THE TIMEOUTS ARE TESTABLE HERE BECAUSE THE CLOCK IS AN ARGUMENT. The AP
 * harness's equivalent retry logic reads a steady_clock, which is why its
 * schedule could only ever be watched on a bench and never asserted. Every
 * cell below that involves a deadline advances `now_ms` by hand.
 *
 * The AP is a fixture: it answers what a real one would, and nothing more. It
 * does not model a station table, retransmissions, or refusal for any reason
 * a cell does not ask for.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "openssl_crypto_ops.h"
#include "sta/BssTable.h"
#include "sta/StationSm.h"

namespace {

using devourer::sta::BssEntry;
using devourer::sta::BssTable;
using devourer::sta::StationSm;
using devourer::test::OpenSslCryptoOps;

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_fail++;
  }
}

const uint8_t kBssid[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x00};
const uint8_t kOwn[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};
const char* kSsid = "devourerAP";
const char* kPsk = "devourer123";

std::vector<uint8_t> beacon(const uint8_t bssid[6], uint8_t chan) {
  static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  std::vector<uint8_t> m =
      devourer::sta::mgmt_hdr(devourer::sta::kFcBeacon, bcast, bssid, bssid);

  m.insert(m.end(), 8, 0);
  devourer::sta::put_le16(m, 100);
  devourer::sta::put_le16(m, 0x0011);            /* ESS | Privacy */
  devourer::sta::append_ssid(m, kSsid);
  devourer::sta::append_supported_rates(m);
  devourer::sta::append_ds_params(m, chan);
  devourer::sta::append_rsn_ccmp_psk(m);
  return m;
}

/* Put a BSS in a table and hand back the entry, so join() is always reached
 * the way a real station reaches it. */
const BssEntry* discovered(BssTable& t, uint8_t chan = 6) {
  std::vector<uint8_t> b = beacon(kBssid, chan);
  return t.observe(b.data(), b.size(), -40, 0);
}

/* ---- the fixture AP ---------------------------------------------------- */

struct FixtureAp {
  OpenSslCryptoOps crypto;
  uint8_t pmk[32] = {0};
  uint8_t anonce[32] = {0};
  uint8_t ptk[48] = {0};
  uint8_t gtk[16] = {0};
  uint64_t replay = 0;
  uint16_t assoc_status = 0;
  uint16_t auth_status = 0;
  uint16_t aid = 3;
  /* A real AP answers a refusal with AID 0, which means the AID check would
   * catch a refusal even if the status check were deleted - and a mutation
   * doing exactly that survived this file. This knob makes the status field
   * the only thing that can refuse. */
  bool aid_even_when_refused = false;
  bool saw_auth = false, saw_assoc = false, saw_msg4 = false;

  FixtureAp() {
    devourer::sta::pmk_from_psk(crypto, kPsk, kSsid, pmk);
    std::memset(anonce, 0x5e, 32);
    std::memset(gtk, 0x31, 16);
  }

  std::vector<uint8_t> mgmt(uint8_t fc) {
    return devourer::sta::mgmt_hdr(fc, kOwn, kBssid, kBssid);
  }

  /* Wrap an EAPOL body in the from-DS data frame a station receives. */
  std::vector<uint8_t> eapol_frame(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> m = devourer::sta::data_hdr_from_ds(
        kOwn, kBssid, kBssid, /*protect=*/false, 1);
    devourer::sta::append_llc_snap(m, 0x888e);
    m.insert(m.end(), body.begin(), body.end());
    return m;
  }

  /* Answer one frame from the station. Returns what the AP would send back,
   * which may be empty. */
  std::vector<uint8_t> respond(const std::vector<uint8_t>& f) {
    if (f.size() < 24) return {};
    const uint8_t fc0 = f[0];

    if (fc0 == devourer::sta::kFcAuth) {
      saw_auth = true;
      std::vector<uint8_t> m = mgmt(devourer::sta::kFcAuth);
      devourer::sta::put_le16(m, 0);             /* open system */
      devourer::sta::put_le16(m, 2);             /* sequence 2 */
      devourer::sta::put_le16(m, auth_status);
      return m;
    }
    if (fc0 == devourer::sta::kFcAssocReq) {
      saw_assoc = true;
      std::vector<uint8_t> m = mgmt(devourer::sta::kFcAssocResp);
      devourer::sta::put_le16(m, 0x0011);
      devourer::sta::put_le16(m, assoc_status);
      devourer::sta::put_le16(
          m, (uint16_t)(assoc_status && !aid_even_when_refused
                            ? 0
                            : (0xc000 | aid)));
      return m;
    }
    if (fc0 != devourer::sta::kFcData) return {};

    /* A data frame from the station: the only one this fixture speaks is
     * EAPOL, and the only messages are 2 and 4. */
    const size_t hlen = 24;
    if (f.size() < hlen + 8 + devourer::sta::kEapolKeyFixedLen) return {};
    devourer::sta::EapolKey k;
    if (!devourer::sta::parse_eapol_key(f.data() + hlen + 8,
                                        f.size() - hlen - 8, &k))
      return {};
    if (!k.secure()) {                            /* message 2 */
      if (!devourer::sta::derive_ptk(crypto, pmk, kBssid, kOwn, anonce,
                                     k.nonce, ptk))
        return {};
      if (!devourer::sta::eapol_mic_ok(crypto, ptk, k)) return {};
      return eapol_frame(msg3());
    }
    saw_msg4 = devourer::sta::eapol_mic_ok(crypto, ptk, k);
    return {};
  }

  std::vector<uint8_t> msg1() {
    replay++;
    return devourer::sta::build_eapol_key(
        devourer::sta::kKeyDescVersionCcmp | devourer::sta::kKiPairwise |
            devourer::sta::kKiAck,
        16, replay, anonce, nullptr, nullptr, 0, nullptr, nullptr);
  }

  std::vector<uint8_t> msg3() {
    std::vector<uint8_t> kd;
    const uint8_t hdr[8] = {0xdd, 0x16, 0x00, 0x0f, 0xac, 0x01, 1, 0x00};

    devourer::sta::append_rsn_ccmp_psk(kd);
    kd.insert(kd.end(), hdr, hdr + 8);
    kd.insert(kd.end(), gtk, gtk + 16);
    if (kd.size() % 8) {
      kd.push_back(0xdd);
      while (kd.size() % 8) kd.push_back(0x00);
    }
    std::vector<uint8_t> w(kd.size() + 8);
    const int n = OpenSslCryptoOps::key_wrap(ptk + 16, 16, kd.data(),
                                             kd.size(), w.data());
    w.resize(n > 0 ? (size_t)n : 0);
    replay++;
    return devourer::sta::build_eapol_key(
        devourer::sta::kKeyDescVersionCcmp | devourer::sta::kKiPairwise |
            devourer::sta::kKiInstall | devourer::sta::kKiAck |
            devourer::sta::kKiMic | devourer::sta::kKiSecure |
            devourer::sta::kKiEncrypted,
        16, replay, anonce, nullptr, w.data(), w.size(), &crypto, ptk);
  }
};

/* Pump: drain the station's transmit queue into the AP, feed the AP's answers
 * back, until nothing moves. `now_ms` does not advance, so nothing here can
 * accidentally depend on a timeout. */
void pump(StationSm& sm, FixtureAp& ap, uint32_t now_ms, int rounds = 8) {
  std::vector<uint8_t> f;

  for (int i = 0; i < rounds; i++) {
    bool moved = false;
    while (sm.pop_tx(&f)) {
      moved = true;
      const std::vector<uint8_t> r = ap.respond(f);
      if (!r.empty()) sm.on_rx(r.data(), r.size(), now_ms);
      /* The AP sends message 1 unprompted once it has associated us. */
      if (f[0] == devourer::sta::kFcAssocReq && ap.assoc_status == 0) {
        const std::vector<uint8_t> m1 = ap.eapol_frame(ap.msg1());
        sm.on_rx(m1.data(), m1.size(), now_ms);
      }
    }
    if (!moved) break;
  }
}

/* ---- cells ------------------------------------------------------------- */

void test_full_association() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  check(sm.configure(crypto, kSsid, kPsk, kOwn, snonce),
        "configure derives the PMK");
  const BssEntry* bss = table.select(kSsid);
  check(bss == nullptr, "nothing is selectable before a beacon");
  discovered(table);
  bss = table.select(kSsid);
  check(bss != nullptr, "the BSS is selectable after one beacon");
  if (!bss) return;

  check(sm.join(*bss, 0), "join starts");
  check(sm.state() == StationSm::State::Authenticating,
        "...in Authenticating");
  check(sm.pending_tx() == 1, "...with an authentication request queued");

  pump(sm, ap, 0);

  check(ap.saw_auth && ap.saw_assoc, "the AP saw both requests");
  check(sm.state() == StationSm::State::Connected, "the station connects");
  check(sm.aid() == ap.aid, "...with the AID the AP allocated");
  check(sm.keyed(), "...and is keyed");
  check(std::memcmp(sm.supplicant().ptk(), ap.ptk, 48) == 0,
        "...on the same PTK the AP derived");
  check(sm.supplicant().gtk_valid() &&
            std::memcmp(sm.supplicant().gtk(), ap.gtk, 16) == 0,
        "...and the GTK the AP sent");
  check(ap.saw_msg4, "the AP's message 4 verified");
  check(sm.auth_tx == 1 && sm.assoc_tx == 1,
        "neither request needed a retransmission");
  check(sm.eapol_rx == 2 && sm.eapol_tx == 2,
        "two EAPOL frames in, two out");
}

/* No AP at all. Three transmissions, then a give-up that names the reason —
 * not a machine that sits in Authenticating forever. */
void test_auth_timeout() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);

  /* Below the deadline nothing happens: a tick is not a retransmission. */
  sm.tick(StationSm::kMgmtTimeoutMs - 1);
  check(sm.auth_tx == 1, "no retransmission before the deadline");

  uint32_t now = 0;
  for (int i = 0; i < 6; i++) {
    now += StationSm::kMgmtTimeoutMs;
    sm.tick(now);
  }
  check(sm.auth_tx == StationSm::kMaxTries,
        "exactly kMaxTries authentication requests are sent");
  check(sm.state() == StationSm::State::Failed, "then it gives up");
  check(sm.fail_reason() == StationSm::Failure::AuthTimeout,
        "...saying which step timed out");

  while (sm.pop_tx(&f)) {
  }
  sm.tick(now + 100000);
  check(sm.pending_tx() == 0, "a failed machine sends nothing further");
}

void test_auth_refused() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  ap.auth_status = 1;                             /* unspecified failure */
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);
  pump(sm, ap, 0);

  check(sm.state() == StationSm::State::Failed, "a refused auth fails");
  check(sm.fail_reason() == StationSm::Failure::AuthRefused, "...as refused");
  check(sm.status() == 1, "...carrying the AP's status code");
  check(!ap.saw_assoc, "...and no association is attempted");
}

void test_assoc_refused() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  ap.assoc_status = 17;                           /* cannot handle more STAs */
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);
  pump(sm, ap, 0);

  check(sm.state() == StationSm::State::Failed, "a refused association fails");
  check(sm.fail_reason() == StationSm::Failure::AssocRefused, "...as refused");
  check(sm.status() == 17, "...carrying status 17, not a generic timeout");
}

/* THE STATUS FIELD IS WHAT REFUSES, not the AID. An AP that answers a
 * refusal with a plausible-looking AID must still be refused, and until this
 * cell existed the AID-zero check was quietly doing the status check's job:
 * deleting the status check entirely changed nothing any test could see. */
void test_assoc_refused_with_a_plausible_aid() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  ap.assoc_status = 12;                           /* denied, unspecified */
  ap.aid_even_when_refused = true;
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);
  pump(sm, ap, 0);

  check(sm.state() == StationSm::State::Failed,
        "a refusal carrying an AID is still a refusal");
  check(sm.fail_reason() == StationSm::Failure::AssocRefused, "...as refused");
  check(sm.status() == 12, "...with the status the AP gave");
  check(sm.aid() == 0, "...and no AID is recorded");
  check(sm.eapol_rx == 0, "...and no handshake is attempted");
}

/* A success status with AID 0 means the AP answered yes without allocating
 * anything. Taking it would leave the station associated with an AID a TIM
 * bitmap cannot index. */
void test_assoc_success_with_zero_aid() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  ap.aid = 0;
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);
  pump(sm, ap, 0);

  check(sm.state() == StationSm::State::Failed,
        "a success response with AID 0 is refused");
  check(sm.aid() == 0, "...and no AID is recorded");
}

void test_deauth_during_handshake() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);

  /* Auth and assoc only: stop before the four-way finishes. */
  while (sm.pop_tx(&f)) {
    const std::vector<uint8_t> r = ap.respond(f);
    if (!r.empty()) sm.on_rx(r.data(), r.size(), 0);
    if (f[0] == devourer::sta::kFcAssocReq) break;
  }
  check(sm.state() == StationSm::State::FourWay, "the four-way is running");

  std::vector<uint8_t> d = devourer::sta::build_deauth(kOwn, kBssid, 7);
  /* build_deauth builds a frame FROM the station; retarget it so it arrives
   * from the AP, which is the direction that matters here. */
  std::memcpy(d.data() + 4, kOwn, 6);
  std::memcpy(d.data() + 10, kBssid, 6);
  std::memcpy(d.data() + 16, kBssid, 6);
  sm.on_rx(d.data(), d.size(), 0);

  check(sm.state() == StationSm::State::Failed, "a deauth ends the attempt");
  check(sm.fail_reason() == StationSm::Failure::Deauthenticated, "...as such");
  check(sm.status() == 7, "...with the reason code the AP gave");
}

/* Every frame must come from the BSS being talked to. Without the addr2
 * check, any AP on the channel drives this machine — including one sending
 * association responses to somebody else. */
void test_frames_from_elsewhere_are_ignored() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);

  /* A perfectly good authentication response, from the wrong AP. */
  std::vector<uint8_t> m = ap.mgmt(devourer::sta::kFcAuth);
  devourer::sta::put_le16(m, 0);
  devourer::sta::put_le16(m, 2);
  devourer::sta::put_le16(m, 0);
  m[10] ^= 0xff;                                  /* addr2: another BSSID */
  sm.on_rx(m.data(), m.size(), 0);
  check(sm.state() == StationSm::State::Authenticating,
        "an auth response from another BSSID does not advance the machine");

  /* And a deauth from a third party must not tear anything down. */
  std::vector<uint8_t> d = devourer::sta::build_deauth(kOwn, kBssid, 7);
  std::memcpy(d.data() + 4, kOwn, 6);
  std::memcpy(d.data() + 10, kBssid, 6);
  d[10] ^= 0xff;
  sm.on_rx(d.data(), d.size(), 0);
  check(sm.state() == StationSm::State::Authenticating,
        "a deauth from another BSSID is ignored");

  /* Addressed to a different station, from the right AP. */
  std::vector<uint8_t> n = ap.mgmt(devourer::sta::kFcAuth);
  devourer::sta::put_le16(n, 0);
  devourer::sta::put_le16(n, 2);
  devourer::sta::put_le16(n, 0);
  n[4] ^= 0xff;                                   /* addr1: someone else */
  sm.on_rx(n.data(), n.size(), 0);
  check(sm.state() == StationSm::State::Authenticating,
        "an auth response for another station is ignored");
}

/* The four-way is never protected. A PROTECTED frame claiming to be EAPOL
 * cannot be one, because the keys it carries are what protection would need. */
void test_protected_eapol_ignored() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);
  while (sm.pop_tx(&f)) {
    const std::vector<uint8_t> r = ap.respond(f);
    if (!r.empty()) sm.on_rx(r.data(), r.size(), 0);
    if (f[0] == devourer::sta::kFcAssocReq) break;
  }

  std::vector<uint8_t> m1 = ap.eapol_frame(ap.msg1());
  m1[1] |= devourer::sta::kFcProtected;
  sm.on_rx(m1.data(), m1.size(), 0);
  check(sm.eapol_rx == 0, "a protected EAPOL frame is not fed to the supplicant");
  check(sm.pending_tx() == 0, "...and nothing is answered");

  /* The same frame unprotected IS accepted — so the cell above is testing the
   * protected bit and not something else about the frame. */
  m1[1] &= (uint8_t)~devourer::sta::kFcProtected;
  sm.on_rx(m1.data(), m1.size(), 0);
  check(sm.eapol_rx == 1, "the same frame unprotected is accepted");
  check(sm.pending_tx() == 1, "...and answered");
}

/* The four-way give-up. The authenticator owns the retransmission schedule,
 * so this side must not sit in FourWay forever when it stops. */
void test_handshake_timeout() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);
  while (sm.pop_tx(&f)) {
    const std::vector<uint8_t> r = ap.respond(f);
    if (!r.empty()) sm.on_rx(r.data(), r.size(), 0);
    if (f[0] == devourer::sta::kFcAssocReq) break;
  }
  check(sm.state() == StationSm::State::FourWay, "the four-way is running");

  sm.tick(StationSm::kHandshakeTimeoutMs - 1);
  check(sm.state() == StationSm::State::FourWay, "it waits out its deadline");
  sm.tick(StationSm::kHandshakeTimeoutMs);
  check(sm.state() == StationSm::State::Failed, "then gives up");
  check(sm.fail_reason() == StationSm::Failure::HandshakeTimeout,
        "...saying the handshake timed out, not the association");
}

/* AN AUTHENTICATOR THAT ONLY RETRANSMITS IS NOT MAKING PROGRESS. Answering a
 * retransmitted message 1 is correct, but letting it push the give-up
 * deadline out means a stuck AP holds this state open forever — a station
 * that never connects and never tries anything else. */
void test_retransmission_does_not_extend_the_deadline() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, 0);
  while (sm.pop_tx(&f)) {
    const std::vector<uint8_t> r = ap.respond(f);
    if (!r.empty()) sm.on_rx(r.data(), r.size(), 0);
    if (f[0] == devourer::sta::kFcAssocReq) break;
  }
  check(sm.state() == StationSm::State::FourWay, "the four-way is running");

  /* The AP's message 1, over and over, at the same replay counter. */
  const std::vector<uint8_t> m1 = ap.eapol_frame(ap.msg1());
  sm.on_rx(m1.data(), m1.size(), 0);
  while (sm.pop_tx(&f)) {
  }
  for (uint32_t t = 500; t < StationSm::kHandshakeTimeoutMs; t += 500) {
    sm.on_rx(m1.data(), m1.size(), t);
    while (sm.pop_tx(&f)) {
    }
    sm.tick(t);
  }
  check(sm.supplicant().retransmits > 0,
        "the retransmissions were seen as retransmissions");
  check(sm.state() == StationSm::State::FourWay,
        "...and the machine is still waiting");

  sm.tick(StationSm::kHandshakeTimeoutMs);
  check(sm.state() == StationSm::State::Failed,
        "the give-up fires on time DESPITE the retransmissions");
  check(sm.fail_reason() == StationSm::Failure::HandshakeTimeout,
        "...as a handshake timeout");
}

/* join() must refuse a BSS this station cannot finish with, rather than
 * authenticating and discovering it three frames later. */
void test_join_refuses_an_unusable_bss() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn, snonce);

  BssEntry open{};
  std::memcpy(open.info.bssid, kBssid, 6);
  open.info.ssid = kSsid;
  open.info.rsn_ccmp_psk = false;
  check(!sm.join(open, 0), "an open BSS is refused by join()");
  check(sm.state() == StationSm::State::Failed, "...and says so");
  check(sm.pending_tx() == 0, "...without sending anything");
}

}  // namespace

int main() {
  test_full_association();
  test_auth_timeout();
  test_auth_refused();
  test_assoc_refused();
  test_assoc_refused_with_a_plausible_aid();
  test_assoc_success_with_zero_aid();
  test_deauth_during_handshake();
  test_frames_from_elsewhere_are_ignored();
  test_protected_eapol_ignored();
  test_handshake_timeout();
  test_retransmission_does_not_extend_the_deadline();
  test_join_refuses_an_unusable_bss();

  if (g_fail) {
    std::printf("station_sm_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("station_sm_selftest: OK\n");
  return 0;
}
