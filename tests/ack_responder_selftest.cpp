/* Headless guard for the ACK-responder recipe (src/AckResponder.h): the arm,
 * the gate-only disarm, and the retarget seam a die needs when the gate alone
 * does not stop its ACK engine.
 *
 * Why retarget() exists. On the RTL8733B a port whose MACID is still
 * programmed keeps auto-ACKing after net_type reads back 0, so
 * Rtl8733bDevice::ClearAckResponder composes disable() with a retarget to the
 * adapter's own MAC. The restore address is deliberately NOT zero: many
 * Realtek MAC TX paths refuse to schedule a frame when the MAC ID is zero
 * (src/jaguar1/HalModule.cpp), and zero would not remove the match anyway —
 * 00:00:00:00:00:00 has the I/G bit clear, so is_unicast() accepts it. Both
 * properties are pinned below.
 *
 * tests/ack_responder_check.sh cannot cover this and never could: every cell
 * in its matrix is a FRESH PROCESS, so its responder-off arm always starts
 * from a chip that was never armed. The behaviour only appears when one
 * process arms and then disarms — which is what this test does against a
 * modelled register file.
 *
 * What it does NOT cover, and cannot:
 *  - whether a retarget stops a given die on air. That is silicon behaviour,
 *    verified on hardware (RTL8733B: 100 % unanswered before an arm, 0.63 %
 *    armed, 100 % again after the composed disarm, 1.15 % re-armed).
 *  - the CALLER's choice of restore address. Nothing here instantiates
 *    Rtl8733bDevice, so a regression that handed retarget() a zeroed buffer
 *    would still pass; the invariant that stops that (_mac_ready implies
 *    EFUSE mac_valid()) lives in ClearAckResponder and initialize().
 *  - the degenerate case where the armed address IS the restore address, in
 *    which no retarget can move the match. SetAckResponder refuses that arm
 *    up front; the refusal is device-layer and untested here. */
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>

#include "AckResponder.h"
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

/* A byte-addressed register file. Only the three fields the recipe touches
 * matter, but modelling the whole space keeps the test honest about WHERE it
 * writes: an implementation that hit the wrong offset would read back zero
 * here rather than quietly passing. */
class FakeRegs final : public devourer::IRtlTransport {
public:
  std::map<uint16_t, uint8_t> mem;
  bool fail_writes = false;
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
    if (fail_writes) return false;
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
};

constexpr uint16_t kNetType = 0x0102;
constexpr uint16_t kMacId = 0x0610;

bool macid_clear(FakeRegs &r) {
  return r.read32(kMacId) == 0 && r.read16(kMacId + 4) == 0;
}

}  // namespace

int main() {
  auto logger = std::make_shared<Logger>();
  const uint8_t mac[6] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a};
  /* Stands in for the adapter's own EFUSE MAC — what MAC bring-up programmed
   * and what the disarm restores. Distinct from the responder MAC, which is
   * the whole point. */
  const uint8_t own[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};

  {
    /* Arm: MACID programmed, gate open, verify() agrees. */
    auto regs = std::make_shared<FakeRegs>();
    RtlAdapter dev(regs, logger);
    CHECK(devourer::ack::enable(dev, mac));
    CHECK(regs->read32(kMacId) == 0x56341202u);
    CHECK(regs->read16(kMacId + 4) == 0x9a78u);
    CHECK((regs->read8(kNetType) & 0x03u) == 0x03u);
    CHECK(devourer::ack::verify(dev, mac));
    CHECK(!devourer::ack::is_disabled(dev));
  }
  {
    /* The portable disarm is gate-only and MUST stay that way: it is what the
     * three unmeasured generations still use, and zeroing or moving their
     * MACID is a change none of them has a bench cell for. */
    auto regs = std::make_shared<FakeRegs>();
    RtlAdapter dev(regs, logger);
    CHECK(devourer::ack::enable(dev, mac));
    CHECK(devourer::ack::disable_verified(dev));
    CHECK((regs->read8(kNetType) & 0x03u) == 0);
    CHECK(regs->read32(kMacId) == 0x56341202u); /* identity left standing */
    CHECK(!devourer::ack::verify(dev, mac));    /* gate shut, so not armed */
  }
  {
    /* The composed disarm the RTL8733B uses: gate closed, then the identity
     * moved off the responder address and onto the adapter's own. */
    auto regs = std::make_shared<FakeRegs>();
    RtlAdapter dev(regs, logger);
    CHECK(devourer::ack::enable(dev, mac));
    CHECK(devourer::ack::disable_verified(dev));
    CHECK(devourer::ack::retarget(dev, own));
    CHECK(devourer::ack::retargeted(dev, own));
    CHECK(!devourer::ack::retargeted(dev, mac)); /* no longer the responder */
    CHECK((regs->read8(kNetType) & 0x03u) == 0);
    /* NOT zero — the T1 state that stops MAC TX scheduling. */
    CHECK(regs->read32(kMacId) != 0 || regs->read16(kMacId + 4) != 0);
    CHECK(regs->read32(kMacId) == 0xccbbaa02u);
    CHECK(regs->read16(kMacId + 4) == 0x01ddu);
  }
  {
    /* Re-arm after a composed disarm restores the responder identity: the
     * live toggle loop must work in both directions without a re-init. */
    auto regs = std::make_shared<FakeRegs>();
    RtlAdapter dev(regs, logger);
    CHECK(devourer::ack::enable(dev, mac));
    CHECK(devourer::ack::disable_verified(dev));
    CHECK(devourer::ack::retarget(dev, own));
    CHECK(devourer::ack::enable(dev, mac));
    CHECK(devourer::ack::verify(dev, mac));
  }
  {
    /* A failing transport must not report a successful retarget, and must have
     * ATTEMPTED both MACID halves — the reason retarget() sequences its writes
     * into locals instead of short-circuiting on `&&`. Note this counts
     * operations without identifying addresses, and its strength depends on
     * FakeRegs::write8 returning BEFORE it stores (so write16/write32
     * short-circuit to one call each): making the fake attempt both bytes
     * would make this assertion tautological. */
    auto regs = std::make_shared<FakeRegs>();
    RtlAdapter dev(regs, logger);
    CHECK(devourer::ack::enable(dev, mac));
    regs->fail_writes = true;
    const int before = regs->write_calls;
    CHECK(!devourer::ack::retarget(dev, own));
    CHECK(regs->write_calls - before == 2); /* both halves attempted */
    regs->fail_writes = false;
  }
  {
    /* The disarm's precondition. Retargeting to the SAME address the port is
     * armed to cannot move the match — both writes and the readback succeed
     * while nothing changes — so the caller must refuse that arm up front
     * rather than emit a success log for a responder that is still answering.
     * Rtl8733bDevice::SetAckResponder gates on exactly this. */
    auto regs = std::make_shared<FakeRegs>();
    RtlAdapter dev(regs, logger);
    CHECK(devourer::ack::disarmable_by_retarget(mac, own));
    CHECK(!devourer::ack::disarmable_by_retarget(own, own));
    /* And the reason it matters, demonstrated on the register file: arming on
     * `own` and then "disarming" to `own` leaves the identity untouched, so
     * retargeted() reports success while the port still matches the address it
     * was armed to. */
    CHECK(devourer::ack::enable(dev, own));
    CHECK(devourer::ack::disable_verified(dev));
    CHECK(devourer::ack::retarget(dev, own));    /* writes what is already there */
    CHECK(devourer::ack::retargeted(dev, own));  /* "succeeds" */
    CHECK(devourer::ack::retargeted(dev, own));  /* still the armed address */
  }
  {
    /* Zero is a UNICAST address by the I/G bit, which is exactly why it is not
     * a safe "no match" value: is_unicast() accepts it, so a peer could
     * solicit it. Pins the reasoning behind restoring a real MAC. */
    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    const uint8_t group[6] = {0x57, 0x42, 0x00, 0x00, 0x13, 0x00};
    CHECK(devourer::ack::is_unicast(mac));
    CHECK(devourer::ack::is_unicast(zero));
    CHECK(!devourer::ack::is_unicast(group));
  }

  if (failures) {
    std::fprintf(stderr, "ack_responder_selftest: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("ack_responder_selftest: OK\n");
  return 0;
}
