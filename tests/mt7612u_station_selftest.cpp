/* Headless guard for the MT7612U station-identity decision and the ownership
 * hand-off (src/mt7612u/StationIdentity.h).
 *
 * Phase 2's plan asked for "a headless selftest of the ownership hand-off in
 * the style of tests/ack_responder_selftest.cpp" and the first implementation
 * did not provide one: the only coverage was `bringup staid`, which needs a
 * device, an AP-free channel and root. Every defect this logic has actually
 * had was a policy decision rather than a register write, so all of it is
 * reachable from here.
 *
 * The two that shipped and were caught in review:
 *
 *   - the MT_AUTO_RSP_EN check failed OPEN on a failed register read, while
 *     the port-identity check beside it failed CLOSED. A stalled EP0 transfer
 *     therefore armed a station whose ability to acknowledge was unknown.
 *   - the port-identity read relied on zero-initialised locals, so a failed
 *     read yielded 00:00:00:00:00:00 and refused for the WRONG reason,
 *     reporting a port identity the MAC never held.
 *
 * Neither is visible in a register trace. Both are one line here.
 *
 * What this does NOT cover: anything about the silicon. Whether the MAC
 * actually receives or acknowledges with a given identity is measured on
 * hardware - docs/mt7612u-station-identity.md, whose retraction section is
 * worth reading before quoting any number from it. */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "StationIdentity.h"

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                              \
    }                                                                          \
  } while (0)

namespace {

constexpr uint32_t kAutoRspEn = 1u << 0;   /* MT_AUTO_RSP_EN */

const uint8_t kOwn[6]   = {0x40, 0xa5, 0xef, 0x5a, 0x32, 0xf8};
const uint8_t kBssid[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0xaa};
const uint8_t kOther[6] = {0x02, 0x00, 0x00, 0xac, 0x1d, 0x01};
const uint8_t kMcast[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
const uint8_t kZero[6]  = {0, 0, 0, 0, 0, 0};

mt7612u_sta_verdict decide(const uint8_t *own, const uint8_t *bssid,
                           const uint8_t *port, int port_ok,
                           uint32_t cfg, int rsp_ok) {
  return mt7612u_sta_decide(own, bssid, port, port_ok, cfg, rsp_ok, kAutoRspEn);
}

void test_the_ordinary_case() {
  CHECK(decide(kOwn, kBssid, kOwn, 1, kAutoRspEn, 1) == MT7612U_STA_OK);
}

void test_malformed_arguments() {
  CHECK(decide(nullptr, kBssid, kOwn, 1, kAutoRspEn, 1) == MT7612U_STA_BAD_ARGS);
  CHECK(decide(kOwn, nullptr, kOwn, 1, kAutoRspEn, 1) == MT7612U_STA_BAD_ARGS);
  CHECK(decide(kMcast, kBssid, kMcast, 1, kAutoRspEn, 1) == MT7612U_STA_MULTICAST);
  CHECK(decide(kOwn, kMcast, kOwn, 1, kAutoRspEn, 1) == MT7612U_STA_MULTICAST);
  CHECK(decide(kOwn, kOwn, kOwn, 1, kAutoRspEn, 1) == MT7612U_STA_SAME_ADDR);

  /* Argument validation must come BEFORE anything that depends on a register
   * read, so a caller mistake is reported as a caller mistake even when the
   * device is unreachable. */
  CHECK(decide(kMcast, kBssid, kOwn, 0, 0, 0) == MT7612U_STA_MULTICAST);
  CHECK(decide(kOwn, kOwn, kOwn, 0, 0, 0) == MT7612U_STA_SAME_ADDR);
}

/* The regression that matters most: BOTH reads must fail CLOSED. An earlier
 * version had them disagree, and the disagreement armed. */
void test_failed_reads_refuse() {
  CHECK(decide(kOwn, kBssid, kOwn, 0, kAutoRspEn, 1) == MT7612U_STA_READ_FAILED);
  CHECK(decide(kOwn, kBssid, kOwn, 1, kAutoRspEn, 0) == MT7612U_STA_READ_FAILED);
  CHECK(decide(kOwn, kBssid, kOwn, 0, 0, 0) == MT7612U_STA_READ_FAILED);

  /* And specifically: a failed AUTO_RSP read must NOT be waved through just
   * because the value that came back happens to look armed. This is the exact
   * shape of the `== 0 && !(rsp & EN)` bug - the read failed, the stale value
   * had the bit set, and the check was skipped. */
  CHECK(decide(kOwn, kBssid, kOwn, 1, kAutoRspEn, 0) != MT7612U_STA_OK);

  /* A failed port read must not be laundered into a mismatch against the
   * all-zero address, which is what zero-initialised locals produced. The
   * verdict has to say the read failed. */
  CHECK(decide(kZero, kBssid, kZero, 0, kAutoRspEn, 1) == MT7612U_STA_READ_FAILED);
}

void test_port_identity_must_be_ours() {
  CHECK(decide(kOwn, kBssid, kOther, 1, kAutoRspEn, 1) == MT7612U_STA_PORT_MISMATCH);
  /* The case the hardware gate covers as case 5: a responder holds the port
   * identity, so arming a station on our own address must be refused. */
  CHECK(decide(kOwn, kBssid, kOther, 1, kAutoRspEn, 1) != MT7612U_STA_OK);
  /* And the reverse - asking to be the address the responder moved it to -
   * is refused too, because that address is not this adapter. */
  CHECK(decide(kOther, kBssid, kOwn, 1, kAutoRspEn, 1) == MT7612U_STA_PORT_MISMATCH);
}

void test_auto_response_engine_must_be_on() {
  CHECK(decide(kOwn, kBssid, kOwn, 1, 0, 1) == MT7612U_STA_AUTO_RSP_OFF);
  /* Other bits set but not the enable is still off. */
  CHECK(decide(kOwn, kBssid, kOwn, 1, 0xfffffffeu, 1) == MT7612U_STA_AUTO_RSP_OFF);
  CHECK(decide(kOwn, kBssid, kOwn, 1, 0xffffffffu, 1) == MT7612U_STA_OK);
}

/* --- the ownership hand-off ---------------------------------------------- */
void test_ownership_handoff() {
  mt7612u_sta_state s{};
  CHECK(s.armed == 0);

  /* Nothing armed: a responder taking the identity is not a loss, and must
   * not produce a diagnostic. */
  CHECK(mt7612u_sta_identity_taken(&s) == 0);

  mt7612u_sta_arm(&s, kBssid);
  CHECK(s.armed == 1);
  CHECK(std::memcmp(s.bssid, kBssid, 6) == 0);

  /* A responder armed AFTERWARDS invalidates the station. This is the
   * ordering the arm-time check cannot help with, and the one a real caller
   * is likelier to hit. */
  CHECK(mt7612u_sta_identity_taken(&s) == 1);
  CHECK(s.armed == 0);
  CHECK(std::memcmp(s.bssid, kZero, 6) == 0);

  /* Idempotent: taking it twice is one loss, not two. A second warning would
   * be noise, and a second "drop" of nothing is not an event. */
  CHECK(mt7612u_sta_identity_taken(&s) == 0);

  /* Re-arming after the responder gives it back works, and the recorded
   * BSSID is the new one rather than a survivor of the previous arm. */
  mt7612u_sta_arm(&s, kOther);
  CHECK(s.armed == 1);
  CHECK(std::memcmp(s.bssid, kOther, 6) == 0);
  mt7612u_sta_clear(&s);
  CHECK(s.armed == 0);
  CHECK(mt7612u_sta_identity_taken(&s) == 0);
}

/* A clear must not leave the previous BSSID readable: the C API returns it to
 * callers, and a stale value would name a BSS this station is not on. */
void test_clear_wipes_the_bssid() {
  mt7612u_sta_state s{};
  mt7612u_sta_arm(&s, kBssid);
  mt7612u_sta_clear(&s);
  CHECK(std::memcmp(s.bssid, kZero, 6) == 0);
}

} // namespace

int main() {
  test_the_ordinary_case();
  test_malformed_arguments();
  test_failed_reads_refuse();
  test_port_identity_must_be_ours();
  test_auto_response_engine_must_be_on();
  test_ownership_handoff();
  test_clear_wipes_the_bssid();

  if (failures) {
    std::fprintf(stderr, "mt7612u_station_selftest: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("mt7612u_station_selftest: all checks passed\n");
  return 0;
}
