/* Headless guard for the Realtek station arm (src/StationArm.h over the
 * AckResponder.h recipe): what IRadio::SetStationIdentity / Clear do to the
 * port-0 registers on Jaguar1/2/3.
 *
 * Pinned here: the arm writes MACID = own, BSSID = the AP, net_type = Infra
 * and nothing else in 0x0102; every refusal writes NOTHING; a port another
 * claimant already owns (net_type set) is refused; a re-arm keeps the FIRST
 * snapshot so Clear returns to the state before any arm; a write that does not
 * land is caught by readback - the BSSID included - and rolled back; and a
 * rollback that cannot verify leaves the arm recorded so Clear can retry it.
 *
 * NOT covered: silicon behaviour. That the arm makes the MAC ACK the AP's
 * unicast is an on-air result (tests/sta_d2d_onair.sh, STA_ARM=1 vs 0). */
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

#include "StationArm.h"
#include "RtlAdapter.h"

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                              \
    }                                                                          \
  } while (0)

namespace {

/* A byte-addressed register file (the ack_responder_selftest shape), plus
 * `deaf`: addresses whose writes report success and store nothing - the
 * failure only a readback can see; `latch`: addresses that keep their FIRST
 * write and ignore the rest - an arm that lands and a rollback that does not;
 * and `open_identity_writes`: identity bytes written while 0x0102 had a
 * net_type set, i.e. a responder live for a half-written address. */
class FakeRegs final : public devourer::ITransport {
public:
  std::map<uint16_t, uint8_t> mem;
  std::set<uint16_t> deaf;
  std::set<uint16_t> latch, latched;
  int open_identity_writes = 0;
  bool fail_writes = false;
  bool throw_writes = false;
  int write_calls = 0;

  bool is_usb() const override { return true; }
  uint8_t read8(uint16_t a) override { return mem.count(a) ? mem[a] : 0; }
  uint16_t read16(uint16_t a) override {
    return static_cast<uint16_t>(read8(a) | (read8(a + 1) << 8));
  }
  uint32_t read32(uint16_t a) override {
    return static_cast<uint32_t>(read16(a)) |
           (static_cast<uint32_t>(read16(a + 2)) << 16);
  }
  bool write8(uint16_t a, uint8_t v) override {
    ++write_calls;
    if (throw_writes) throw std::runtime_error("injected write failure");
    if (fail_writes) return false;
    if (a >= 0x0610 && a <= 0x061d && (read8(0x0102) & 0x03u) != 0)
      ++open_identity_writes;
    if (deaf.count(a)) return true;
    if (latch.count(a)) {
      if (latched.count(a)) return true;
      latched.insert(a);
    }
    mem[a] = v;
    return true;
  }
  bool write16(uint16_t a, uint16_t v) override {
    return write8(a, static_cast<uint8_t>(v)) &&
           write8(a + 1, static_cast<uint8_t>(v >> 8));
  }
  bool write32(uint16_t a, uint32_t v) override {
    return write16(a, static_cast<uint16_t>(v)) &&
           write16(a + 2, static_cast<uint16_t>(v >> 16));
  }
  bool write_bytes(uint16_t, const uint8_t *, size_t) override { return true; }
  bool tx_async(uint8_t, uint8_t *, size_t, unsigned) override { return true; }
  int tx_sync(uint8_t, uint8_t *, size_t len, int) override {
    return static_cast<int>(len);
  }
  void rx_loop(int, int, const std::function<void(const uint8_t *, int)> &,
               const std::function<bool()> &) override {}

  void put_mac(uint16_t at, const uint8_t m[6]) {
    for (int i = 0; i < 6; ++i) mem[at + i] = m[i];
  }
  bool mac_is(uint16_t at, const uint8_t m[6]) {
    for (int i = 0; i < 6; ++i)
      if (read8(at + i) != m[i]) return false;
    return true;
  }
};

constexpr uint16_t kNetType = 0x0102;
constexpr uint16_t kMacId = 0x0610;
constexpr uint16_t kBssid = 0x0618;

const uint8_t kOwn[6] = {0x40, 0xa5, 0xef, 0x2f, 0x22, 0x9b};
const uint8_t kAp[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x00};
const uint8_t kAp2[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x01};
/* The pre-arm port: what bring-up left (a nonzero MACID, a stale BSSID) and
 * 0x0102 with upper bits set and net_type NoLink - upper bits the arm must
 * carry through untouched. */
const uint8_t kPreMac[6] = {0x00, 0xe0, 0x4c, 0x88, 0x22, 0xbb};
const uint8_t kPreBss[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t kPreNetReg = 0xa4;

devourer::MacAddr mac(const uint8_t m[6]) {
  return devourer::MacAddr{{m[0], m[1], m[2], m[3], m[4], m[5]}};
}

std::shared_ptr<FakeRegs> fresh_port() {
  auto r = std::make_shared<FakeRegs>();
  r->put_mac(kMacId, kPreMac);
  r->put_mac(kBssid, kPreBss);
  r->mem[kNetType] = kPreNetReg;
  return r;
}

bool at_pre_arm(FakeRegs &r) {
  return r.mac_is(kMacId, kPreMac) && r.mac_is(kBssid, kPreBss) &&
         r.read8(kNetType) == kPreNetReg;
}

}  // namespace

int main() {
  auto log = std::make_shared<Logger>();
  log->set_level(Logger::Level::Silent);

  { /* arm, then clear: the exact registers, both ways */
    auto r = fresh_port();
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    CHECK(s.armed());
    CHECK(r->mac_is(kMacId, kOwn));
    CHECK(r->mac_is(kBssid, kAp));
    CHECK(r->read8(kNetType) == ((kPreNetReg & ~0x03u) | 0x02u));
    CHECK(s.clear(dev, log, "t"));
    CHECK(!s.armed());
    CHECK(at_pre_arm(*r));
  }
  { /* every argument refusal writes nothing */
    const uint8_t grp[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
    for (int c = 0; c < 3; ++c) {
      auto r = fresh_port();
      RtlAdapter dev(r, log);
      devourer::StationArm s;
      const uint8_t *o = c == 0 ? grp : kOwn;
      const uint8_t *b = c == 1 ? grp : (c == 2 ? kOwn : kAp);
      CHECK(!s.arm(dev, mac(o), mac(b), log, "t"));
      CHECK(!s.armed());
      CHECK(r->write_calls == 0);
      CHECK(at_pre_arm(*r));
    }
  }
  { /* a port someone else owns (AP net_type) is refused, untouched */
    auto r = fresh_port();
    r->mem[kNetType] = 0xa7;
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(!s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    CHECK(!s.armed());
    CHECK(r->write_calls == 0);
    CHECK(r->read8(kNetType) == 0xa7);
  }
  { /* a re-arm on a new BSSID keeps the FIRST snapshot, and closes the gate
     * before it moves the identity - the port is live (Infra) at that point */
    auto r = fresh_port();
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    CHECK(r->open_identity_writes == 0);
    CHECK(s.arm(dev, mac(kOwn), mac(kAp2), log, "t"));
    CHECK(r->open_identity_writes == 0);
    CHECK(r->mac_is(kBssid, kAp2));
    CHECK(s.clear(dev, log, "t"));
    CHECK(at_pre_arm(*r));
  }
  { /* a BSSID write that reports success and stores nothing: caught by
     * readback, rolled back, not armed */
    auto r = fresh_port();
    for (int i = 0; i < 6; ++i) r->deaf.insert(kBssid + i);
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(!s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    CHECK(!s.armed());
    CHECK(at_pre_arm(*r));
  }
  { /* a net_type write that does not land: not armed, rolled back */
    auto r = fresh_port();
    r->deaf.insert(kNetType);
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(!s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    CHECK(!s.armed());
    CHECK(at_pre_arm(*r));
  }
  { /* writes failing outright: false, and since the rollback cannot land
     * either the arm stays recorded - Clear retries it once the bus is back */
    auto r = fresh_port();
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    r->deaf.insert(kMacId);  /* the rollback's MACID restore will not land */
    CHECK(!s.clear(dev, log, "t"));
    CHECK(s.armed());
    r->deaf.clear();
    CHECK(s.clear(dev, log, "t"));
    CHECK(!s.armed());
    CHECK(at_pre_arm(*r));
  }
  { /* the arm fails readback AND its rollback cannot land: the arm must
     * stay recorded, or Clear has nothing to retry and the port is left
     * answering for `own` with nobody holding the restore target */
    auto r = fresh_port();
    for (int i = 0; i < 6; ++i) r->deaf.insert(kBssid + i);  /* arm fails */
    r->latch.insert(kMacId);  /* MACID takes `own`, refuses the restore */
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(!s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    CHECK(s.armed());
    CHECK(!s.clear(dev, log, "t"));
    CHECK(s.armed());
    r->latch.clear();
    CHECK(s.clear(dev, log, "t"));
    CHECK(!s.armed());
    CHECK(at_pre_arm(*r));
  }
  { /* a throwing transport: no exception escapes, not armed */
    auto r = fresh_port();
    r->throw_writes = true;
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    bool threw = false;
    try {
      CHECK(!s.arm(dev, mac(kOwn), mac(kAp), log, "t"));
    } catch (...) {
      threw = true;
    }
    CHECK(!threw);
  }
  { /* ack::restore_station on its own: it restores the snapshot's net_type
     * bits, not merely "closed". Through StationArm the snapshot is always
     * NoLink (the ownership refusal), which makes that half unobservable
     * there - so the helper's own contract is pinned here directly. */
    auto r = fresh_port();
    RtlAdapter dev(r, log);
    devourer::ack::StationRestore saved;
    CHECK(devourer::ack::snapshot_station_restore(dev, saved));
    saved.net_type = 0x01; /* Ad-hoc, as a snapshot might have recorded */
    CHECK(devourer::ack::arm_station(dev, kOwn, kAp));
    CHECK(devourer::ack::restore_station(dev, saved));
    CHECK(r->read8(kNetType) == ((kPreNetReg & ~0x03u) | 0x01u));
    CHECK(r->mac_is(kMacId, kPreMac));
  }
  { /* clear with nothing armed: true, and touches nothing */
    auto r = fresh_port();
    RtlAdapter dev(r, log);
    devourer::StationArm s;
    CHECK(s.clear(dev, log, "t"));
    CHECK(r->write_calls == 0);
  }

  if (failures) {
    std::fprintf(stderr, "station_arm_selftest: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("station_arm_selftest: ok\n");
  return 0;
}
