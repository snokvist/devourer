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

std::vector<uint8_t> beacon(const uint8_t bssid[6], uint8_t chan,
                            bool rsn = true) {
  static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  std::vector<uint8_t> m =
      devourer::sta::mgmt_hdr(devourer::sta::kFcBeacon, bcast, bssid, bssid);

  m.insert(m.end(), 8, 0);
  devourer::sta::put_le16(m, 100);
  /* Privacy tracks the RSN element. An open BSS that still set the bit would
   * be refused by the open path for the right reason by accident, which is
   * the sort of agreement that makes a cell unfalsifiable. */
  devourer::sta::put_le16(m, (uint16_t)(rsn ? 0x0011 : 0x0001));
  devourer::sta::append_ssid(m, kSsid);
  devourer::sta::append_supported_rates(m);
  devourer::sta::append_ds_params(m, chan);
  if (rsn) devourer::sta::append_rsn_ccmp_psk(m);
  return m;
}

/* Put a BSS in a table and hand back the entry, so join() is always reached
 * the way a real station reaches it. */
const BssEntry* discovered(BssTable& t, uint8_t chan = 6, bool rsn = true) {
  std::vector<uint8_t> b = beacon(kBssid, chan, rsn);
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
  /* An AP on an OPEN BSS sends no message 1. The knob exists so an open cell
   * can choose either: quiet, which is what a real open AP does, or noisy,
   * which is the configuration mismatch an open station must survive. */
  bool sends_msg1 = true;
  bool saw_auth = false, saw_assoc = false, saw_msg4 = false;
  /* The association request as it went out, so a cell can read the bytes
   * rather than infer them from the outcome. */
  std::vector<uint8_t> last_assoc;

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
      last_assoc = f;
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
      if (f[0] == devourer::sta::kFcAssocReq && ap.assoc_status == 0 &&
          ap.sends_msg1) {
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
  check(sm.configure(crypto, kSsid, kPsk, kOwn),
        "configure derives the PMK");
  const BssEntry* bss = table.select(kSsid);
  check(bss == nullptr, "nothing is selectable before a beacon");
  discovered(table);
  bss = table.select(kSsid);
  check(bss != nullptr, "the BSS is selectable after one beacon");
  if (!bss) return;

  check(sm.join(*bss, snonce, 0), "join starts");
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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);

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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  ap.auth_status = 1;                             /* unspecified failure */
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  ap.assoc_status = 17;                           /* cannot handle more STAs */
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  ap.assoc_status = 12;                           /* denied, unspecified */
  ap.aid_even_when_refused = true;
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  ap.aid = 0;
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);

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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);

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
  /* THE LAST OCTET of addr1. Flipping byte 0 sets the group bit and makes the
   * frame a BROADCAST, which is addressed to this station as much as to
   * anyone - it then survives the unicast filter and is dropped only by the
   * `if (to_us)` inside the auth branch, so deleting the filter would not
   * change the outcome. This cell asserted exactly that until 2026-09-21. */
  const uint32_t before = sm.rx_not_for_us;
  n[9] ^= 0xff;
  sm.on_rx(n.data(), n.size(), 0);
  check(sm.state() == StationSm::State::Authenticating,
        "an auth response for another station is ignored");
  check(sm.rx_not_for_us == before + 1,
        "...by the address filter, which counted it");
}

/* The four-way is never protected. A PROTECTED frame claiming to be EAPOL
 * cannot be one, because the keys it carries are what protection would need. */
/* A protected data frame is the caller's to decrypt, and is counted apart
 * from a protocol error. It used to land in rx_ignored, which on a working
 * link is EVERY data frame - a 60-second on-air run carrying 75 frames
 * reported ignored=75, and that counter set is the one thing that answers
 * "why did nothing associate". */
void test_protected_data_is_counted_apart() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);
  discovered(table);
  const BssEntry* bss = table.select(kSsid);
  if (!bss) { check(false, "BSS discovered"); return; }
  sm.join(*bss, snonce, 0);
  pump(sm, ap, 0);
  check(sm.state() == StationSm::State::Connected, "connected");

  const uint32_t ignored = sm.rx_ignored;
  std::vector<uint8_t> f = devourer::sta::data_hdr_from_ds(
      kOwn, kBssid, kBssid, /*protect=*/true, 7);
  f.insert(f.end(), 40, 0x11);
  sm.on_rx(f.data(), f.size(), 0);
  check(sm.rx_protected == 1, "a protected data frame is counted as protected");
  check(sm.rx_ignored == ignored, "...and NOT as ignored");
}

void test_protected_eapol_ignored() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
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
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
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

/* JOINING A SECOND BSS MUST NOT AIR THE FIRST ONE'S FRAMES.
 *
 * join() reset everything except the transmit queue, so auth requests still
 * queued for the BSS we gave up on went out at the one we just joined -
 * addressed to the old BSSID, after the radio had retuned to the new channel.
 * Every other cell here drains the queue between steps, which is exactly why
 * none of them saw it. */
void test_join_clears_the_transmit_queue() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  uint8_t snonce[32];
  std::vector<uint8_t> f;
  const uint8_t kOther[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x99};

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);

  /* Three unanswered authentication requests pile up, unread. */
  uint32_t now = 0;
  for (int i = 0; i < 2; i++) { now += StationSm::kMgmtTimeoutMs; sm.tick(now); }
  check(sm.pending_tx() == 3, "three auth requests are queued for the first BSS");

  BssEntry other = *bss;
  std::memcpy(other.info.bssid, kOther, 6);
  sm.join(other, snonce, now);
  check(sm.pending_tx() == 1,
        "join() clears the queue - only the new BSS's request is pending");
  check(sm.pop_tx(&f) && std::memcmp(f.data() + 4, kOther, 6) == 0,
        "...and it is addressed to the BSS we actually joined");
}

/* THE QUEUE IS BOUNDED. Every frame in it is produced in answer to a received
 * one, so an unbounded queue is an unbounded allocation an attacker controls:
 * one captured EAPOL frame replayed at the current counter is answered every
 * time. */
void test_transmit_queue_is_bounded() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
  while (sm.pop_tx(&f)) {
    const std::vector<uint8_t> r = ap.respond(f);
    if (!r.empty()) sm.on_rx(r.data(), r.size(), 0);
    if (f[0] == devourer::sta::kFcAssocReq) break;
  }
  const std::vector<uint8_t> m1 = ap.eapol_frame(ap.msg1());
  sm.on_rx(m1.data(), m1.size(), 0);

  /* The caller never drains. An attacker replays the same frame. */
  for (int i = 0; i < 2000; i++) sm.on_rx(m1.data(), m1.size(), 0);
  check(sm.pending_tx() <= StationSm::tx_capacity(),
        "the transmit queue never exceeds its capacity");
  check(sm.tx_dropped > 0, "...and the drops are counted, not silent");
}

/* CONNECTED HAS AN EXIT. Without beacon supervision the only way out is a
 * deauth from an AP that may have been switched off, and the caller sees
 * keyed() forever. */
void test_beacon_loss() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
  pump(sm, ap, 0);
  check(sm.state() == StationSm::State::Connected, "the station connects");

  /* Beacons keep arriving: nothing happens, however long it runs. */
  uint32_t now = 0;
  for (int i = 0; i < 20; i++) {
    now += StationSm::kBeaconLossMs / 2;
    std::vector<uint8_t> b = beacon(kBssid, 6);
    sm.on_rx(b.data(), b.size(), now);
    sm.tick(now);
  }
  check(sm.state() == StationSm::State::Connected,
        "a beaconing AP keeps the station connected indefinitely");
  check(sm.beacons_rx == 20, "...and the beacons are counted");

  /* They stop. */
  sm.tick(now + StationSm::kBeaconLossMs - 1);
  check(sm.state() == StationSm::State::Connected, "...it waits out the window");
  sm.tick(now + StationSm::kBeaconLossMs);
  check(sm.state() == StationSm::State::Failed, "a silent AP ends the link");
  check(sm.fail_reason() == StationSm::Failure::BeaconLost,
        "...saying the beacon was lost, not that the handshake timed out");
  check(!sm.keyed(), "...and keyed() stops claiming a link that is gone");
}

/* leave() tells the AP rather than letting it time the station out - which on
 * this project's own AP holds an AID and one of seven table slots. */
void test_leave() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);
  pump(sm, ap, 0);
  check(sm.state() == StationSm::State::Connected, "the station connects");

  sm.leave(3);
  check(sm.state() == StationSm::State::Idle, "leave() goes back to Idle");
  check(!sm.keyed(), "...and drops the keys");
  check(sm.aid() == 0, "...and the AID");
  check(sm.pop_tx(&f), "...having queued a frame");
  check(f[0] == devourer::sta::kFcDeauth, "...which is a deauthentication");
  check(std::memcmp(f.data() + 4, kBssid, 6) == 0, "...addressed to the AP");
  sm.leave(3);
  check(sm.pending_tx() == 0, "leaving twice sends nothing the second time");
}

/* The RX filter counts what it discards. On hardware this is the only address
 * filter in the system, so "it did not associate" must come with a number
 * saying whether the AP was ever heard. */
void test_rx_counters() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  FixtureAp ap;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);
  const BssEntry* bss = discovered(table);
  if (!bss) { check(false, "beacon"); return; }
  sm.join(*bss, snonce, 0);

  std::vector<uint8_t> m = ap.mgmt(devourer::sta::kFcAuth);
  devourer::sta::put_le16(m, 0);
  devourer::sta::put_le16(m, 2);
  devourer::sta::put_le16(m, 0);

  std::vector<uint8_t> foreign = m;
  foreign[10] ^= 0xff;
  sm.on_rx(foreign.data(), foreign.size(), 0);
  check(sm.rx_not_our_bss == 1, "a frame from another BSS is counted");

  std::vector<uint8_t> elsewhere = m;
  /* The LAST octet of addr1, not the first: flipping byte 0 sets the
   * group bit and turns the frame into a broadcast, which is addressed to
   * this station as much as to anyone. The first version of this cell did
   * exactly that and measured the wrong counter. */
  elsewhere[9] ^= 0xff;
  sm.on_rx(elsewhere.data(), elsewhere.size(), 0);
  check(sm.rx_not_for_us == 1, "a frame for another station is counted");

  /* Somebody else's data traffic, correctly addressed to us: not an error,
   * but it must not be confused with one. */
  std::vector<uint8_t> d = devourer::sta::data_hdr_from_ds(
      kOwn, kBssid, kBssid, /*protect=*/false, 1);
  devourer::sta::append_llc_snap(d, 0x0800);
  d.insert(d.end(), 20, 0x41);
  sm.on_rx(d.data(), d.size(), 0);
  check(sm.rx_ignored == 1, "a non-EAPOL data frame is counted separately");
  check(sm.rx_not_our_bss == 1 && sm.rx_not_for_us == 1,
        "...and does not move the address counters");
}

/* join() must refuse a BSS this station cannot finish with, rather than
 * authenticating and discovering it three frames later. */
void test_join_refuses_an_unusable_bss() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  sm.configure(crypto, kSsid, kPsk, kOwn);

  BssEntry open{};
  std::memcpy(open.info.bssid, kBssid, 6);
  open.info.ssid = kSsid;
  open.info.rsn_ccmp_psk = false;
  check(!sm.join(open, snonce, 0), "an open BSS is refused by join()");
  check(sm.state() == StationSm::State::Failed, "...and says so");
  check(sm.pending_tx() == 0, "...without sending anything");
}

/* A CryptoOps whose PBKDF2 refuses, which is the only way to reach the NoPmk
 * branch: OpenSSL's does not fail for any passphrase a caller can supply.
 * Without it that branch is unreachable from this file and deleting it costs
 * nothing - which is what "the test could not fail" means. */
struct NoPbkdf2Crypto : OpenSslCryptoOps {
  bool pbkdf2_sha1(const char*, const uint8_t*, size_t, unsigned, uint8_t*,
                   size_t) override {
    return false;
  }
};

/* configure() reports the failure, and join() must then name it NoPmk rather
 * than authenticating at an AP it can never finish a handshake with. */
void test_no_pmk() {
  NoPbkdf2Crypto crypto;
  BssTable table;
  StationSm sm;
  uint8_t snonce[32];

  std::memset(snonce, 0x7a, 32);
  check(!sm.configure(crypto, kSsid, kPsk, kOwn),
        "configure reports a failed PMK derivation");
  discovered(table);
  const BssEntry* bss = table.select(kSsid);
  if (!bss) { check(false, "BSS discovered"); return; }
  check(!sm.join(*bss, snonce, 0), "join refuses");
  check(sm.fail_reason() == StationSm::Failure::NoPmk,
        "...naming NoPmk, not NotConfigured");
  check(sm.pending_tx() == 0, "...without airing an authentication request");
}

/* ---- the open-network path (Phase 4) ----------------------------------
 *
 * WHY IT EXISTS: without it a station that never reaches Connected cannot
 * say whether the failure is in authentication/association or in the key
 * exchange, because on a WPA2 BSS the two halves come up together or not at
 * all. Phase 4's on-air harness runs an `open` cell first for exactly that
 * reason, mirroring the AP side's ap_responder/ap_wpa2 ladder.
 */
void test_open_association() {
  BssTable table;
  StationSm sm;
  FixtureAp ap;

  ap.sends_msg1 = false;                 /* a real open AP keys nothing */
  check(sm.configure_open(kSsid, kOwn), "configure_open succeeds");
  check(sm.security() == StationSm::Security::Open, "...and says it is open");

  discovered(table, 6, /*rsn=*/false);
  const BssEntry* bss = table.select_open(kSsid);
  check(bss != nullptr, "an open BSS is selectable by select_open");
  check(table.select(kSsid) == nullptr,
        "...and NOT by select(), which wants WPA2-PSK");
  if (!bss) return;

  /* A null SNonce, deliberately: the open path must not read it. Passing a
   * real one would leave a mutation that deleted the Open branch of the copy
   * undetectable. */
  check(sm.join(*bss, nullptr, 0), "join starts without an SNonce");
  pump(sm, ap, 0);

  check(ap.saw_auth && ap.saw_assoc, "the AP saw both requests");
  check(sm.state() == StationSm::State::Connected,
        "the station connects with no four-way");
  check(sm.connected(), "connected() is true");
  check(!sm.keyed(), "...and keyed() is FALSE - there is no key");
  check(sm.aid() == ap.aid, "...with the AID the AP allocated");
  check(sm.eapol_tx == 0 && sm.eapol_rx == 0, "no EAPOL in either direction");
  check(sm.supplicant().state() == devourer::sta::Supplicant::State::Idle,
        "the supplicant was never started");

  /* THE WIRE BYTES, not the outcome. An association request that still
   * carried the RSN element, or still claimed Privacy, would associate
   * against this fixture exactly as happily - the fixture does not look - and
   * would be refused by a real open AP. */
  check(!ap.last_assoc.empty(), "the association request was captured");
  if (ap.last_assoc.size() >= 28) {
    const uint16_t cap = devourer::sta::get_le16(ap.last_assoc.data() + 24);
    size_t ie_len = 0;
    check((cap & 0x0010) == 0, "...with the Privacy capability bit CLEAR");
    check((cap & 0x0001) != 0, "...and ESS still set");
    check(devourer::sta::find_ie(ap.last_assoc.data() + 28,
                                 ap.last_assoc.size() - 28,
                                 devourer::sta::kEidRsn, &ie_len) == nullptr,
          "...and no RSN element");
  }
}

/* An open station on a BSS that encrypts. It must refuse before
 * authenticating: associating would succeed and every data frame would then
 * be dropped by one side or the other, with no diagnostic anywhere. */
void test_open_station_refuses_a_protected_bss() {
  BssTable table;
  StationSm sm;

  sm.configure_open(kSsid, kOwn);
  discovered(table, 6, /*rsn=*/true);
  check(table.select_open(kSsid) == nullptr,
        "select_open skips a BSS that advertises Privacy");

  /* select_open refusing is not enough - join() is reachable with a
   * hand-picked entry, which is how a caller with a configured BSSID gets
   * here. Both gates are tested because either alone can be deleted. */
  BssEntry e{};
  std::memcpy(e.info.bssid, kBssid, 6);
  e.info.ssid = kSsid;
  e.info.privacy = true;
  check(!sm.join(e, nullptr, 0), "join() refuses it too");
  check(sm.state() == StationSm::State::Failed, "...and says so");
  check(sm.pending_tx() == 0, "...without airing an authentication request");
}

/* The configuration mismatch: an open station at an AP that tries to key it.
 * The supplicant holds no PMK and cannot verify a MIC, so feeding it an
 * EAPOL-Key frame would either crash or invent a reply. It is counted and
 * dropped, and the link stays up. */
void test_open_station_ignores_eapol() {
  BssTable table;
  StationSm sm;
  FixtureAp ap;

  ap.sends_msg1 = true;                  /* the mismatch, on purpose */
  sm.configure_open(kSsid, kOwn);
  discovered(table, 6, /*rsn=*/false);
  const BssEntry* bss = table.select_open(kSsid);
  if (!bss) { check(false, "open BSS discovered"); return; }
  sm.join(*bss, nullptr, 0);
  pump(sm, ap, 0);

  const uint32_t ignored_before = sm.rx_ignored;
  const std::vector<uint8_t> m1 = ap.eapol_frame(ap.msg1());
  sm.on_rx(m1.data(), m1.size(), 0);

  check(sm.rx_ignored > ignored_before, "the EAPOL-Key frame is counted");
  check(sm.eapol_rx == 0, "...and never reaches the supplicant");
  check(sm.pending_tx() == 0, "...and is not answered");
  check(sm.state() == StationSm::State::Connected, "...and the link stays up");
}

/* A WPA2 join with no SNonce. The station used to memcpy 32 bytes from
 * whatever the caller passed; a caller that configures once and joins
 * repeatedly would have reused the PREVIOUS association's nonce, which is the
 * defect configure()'s comment already warns about from the other direction. */
void test_wpa2_join_needs_an_snonce() {
  OpenSslCryptoOps crypto;
  BssTable table;
  StationSm sm;

  sm.configure(crypto, kSsid, kPsk, kOwn);
  discovered(table);
  const BssEntry* bss = table.select(kSsid);
  if (!bss) { check(false, "BSS discovered"); return; }
  check(!sm.join(*bss, nullptr, 0), "a WPA2 join without an SNonce is refused");
  check(sm.fail_reason() == StationSm::Failure::NotConfigured,
        "...as NotConfigured");
  check(sm.pending_tx() == 0, "...without airing anything");
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
  test_protected_data_is_counted_apart();
  test_handshake_timeout();
  test_retransmission_does_not_extend_the_deadline();
  test_join_clears_the_transmit_queue();
  test_transmit_queue_is_bounded();
  test_beacon_loss();
  test_leave();
  test_rx_counters();
  test_join_refuses_an_unusable_bss();
  test_no_pmk();
  test_open_association();
  test_open_station_refuses_a_protected_bss();
  test_open_station_ignores_eapol();
  test_wpa2_join_needs_an_snonce();

  if (g_fail) {
    std::printf("station_sm_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("station_sm_selftest: OK\n");
  return 0;
}
