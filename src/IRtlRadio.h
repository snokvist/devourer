#ifndef IRTL_RADIO_H
#define IRTL_RADIO_H

#include "IRadio.h"

#include "AdapterHealth.h" /* EfuseStability */
#include "RxSense.h"       /* RxEnergy */

/* IRtlRadio is the Realtek-family extension of IRadio: the members whose
 * semantics are defined by Realtek silicon (phydm false-alarm / CCA / IGI / NHM
 * counters, the EFUSE logical map and its 0x8129 EEPROM id, the AFE crystal-cap
 * register, the rtw canary register set) rather than by a vendor-neutral
 * concept. Every Realtek backend derives from it:
 *   - RtlJaguarDevice   — Realtek "Jaguar" wave-1 (8812AU/8811AU/8821AU/8814AU)
 *   - RtlJaguar2Device  — Realtek "Jaguar2" (8822BU/8812BU)
 *   - RtlJaguar3Device  — Realtek "Jaguar3" (8822CU/8812EU/8822EU)
 *   - Rtl8733bDevice    — Realtek HALMAC 87xx 11n (RTL8731BU/RTL8733BU)
 *   - RtlKestrelDevice  — Realtek G6 11ax (RTL8852BU/RTL8852CU)
 *
 * WiFiDriver::CreateRadio returns an IRadio; a caller that needs one of these
 * members dynamic_casts to IRtlRadio and treats nullptr as "not a Realtek
 * radio" — skip the feature with one diagnostic, never fake a reading.
 * Per-generation research helpers (BB-debug-port reads, the 8814 queue poller,
 * the CW tone) stay on the concrete classes: the same convention one level
 * further down. Every member here keeps the IRadio rule — virtual with a
 * not-ported default, never pure.
 *
 * One member here is the other direction: GetChannelBusy is DECLARED on IRadio
 * (it is a vendor-neutral concept) and merely IMPLEMENTED here, once, in terms
 * of this family's own facility. The split is about where a member is
 * declared, not where a family implements it — so this is not a re-mixing of
 * it, and it saves five identical per-backend overrides. */
class IRtlRadio : public IRadio {
public:
  /* Crystal (XTAL) load-capacitance trim — the CFO lever. Writes the AFE
   * crystal-cap field (a per-chip register), pulling the chip's reference
   * oscillator a few ppm to align a marginal TX/RX crystal pair; the payoff
   * is narrowband at the edge of its CFO budget (5 MHz at 5 GHz). `cap` is a
   * raw trim code in [0, GetAdapterCaps().xtal_cap_max]; cap < 0 reverts to
   * the efuse/default value. Both physical caps (Xi/Xo) are set together.
   * Returns the applied code, or -1 when unsupported. Sticky across channel
   * changes (an AFE register, untouched by the RF retune). */
  virtual int SetXtalCap(int cap) {
    (void)cap;
    return -1;
  }

  /* Current crystal-cap code (the last SetXtalCap value, or the efuse default
   * at bring-up). -1 when unsupported. */
  virtual int GetXtalCap() { return -1; }

  /* Frame-free RX energy / channel-busy snapshot (see RxSense.h) — the read side
   * of the DEVOURER_CW_TONE emitter, used for spectrum-sensing / interferer
   * detection. Reads the chip's phydm false-alarm + CCA counters, DIG/IGI, and
   * (when asked) the NHM power histogram. FA/CCA counts are the delta since the
   * previous call. Default returns an all-invalid snapshot; each generation
   * overrides with a real reader.
   *
   * `with_nhm` is a cost decision, not a preference: the NHM read arms a ~2 ms
   * measurement window and then polls a ready bit at 1 ms granularity
   * (src/NhmReader.h), so it dominates the call — the scalar FA/CCA/IGI path is
   * a handful of register reads. Pass false for the throwaway read that resets
   * the delta counters before an observation window, and for any caller
   * sampling faster than a few times a second. */
  virtual RxEnergy GetRxEnergy(bool with_nhm) { (void)with_nhm; return {}; }

  /* The neutral reading (IRadio::GetChannelBusy), expressed in this family's
   * own facility: CLM busy airtime plus NHM-env, both of which ride
   * GetRxEnergy's armed CCX window.
   *
   * Implemented ONCE here rather than five times because every Realtek
   * backend's answer is the same function of its GetRxEnergy. A generation
   * that overrides GetRxEnergy gets this for free; one that does not gets a
   * correct "no reading" instead of a fabricated zero — which is the right
   * answer for the RTL8733B (no override at all) and for Kestrel (whose
   * GetRxEnergy fills only the absolute noise floor: its NHM rides the halbb
   * glue, not NhmReader, so there is no CLM).
   *
   * Left non-final on purpose: CLM's period field is independent of the NHM
   * window and reaches ~262 ms, so a generation may later want a longer,
   * lower-variance window than the shared read gives.
   *
   * COST + CONTENTION: see the IRadio declaration. This arms the ~2 ms NHM
   * window and consumes the same delta GetRxEnergy and GetRxQuality read. */
  devourer::ChannelBusy GetChannelBusy() override {
    return devourer::busy_from_rx_energy(GetRxEnergy(/*with_nhm=*/true));
  }

  /* Perform `reads` fresh PHYSICAL EFUSE logical-map reads (each pass re-runs
   * the efuse-controller read sequence — not the cached shadow) and
   * cross-compare them. Dying silicon returns different content per read;
   * healthy silicon is byte-identical every time. Post-bring-up only: returns
   * supported=false before Init/InitWrite (on the 8814AU a pre-fwdl EFUSE
   * read breaks the RSVD-page firmware download). Control-plane threading
   * contract applies (same as SetMonitorChannel). */
  virtual devourer::EfuseStability ProbeEfuseStability(int reads = 4) {
    (void)reads;
    return {};
  }

  /* Dump the chip's canary register set (BB / MAC / per-path RF) to the
   * diagnostic plane. Reads only — no writes, no calibration, no bring-up.
   *
   * The point is that it is callable on a device that has NOT been Init'ed, so
   * a chip left in whatever state a previous session abandoned it in can be
   * inspected AS IT IS. Every other path into this driver reconfigures the chip
   * on the way in, which destroys exactly the evidence a state bug leaves
   * behind. Pair it with an open that skips libusb_reset_device
   * (claim_interface_then_reset's `do_reset=false`) — a USB reset re-runs the
   * chip's own boot and is just as destructive.
   *
   * Output format matches DEVOURER_DUMP_CANARY, so two dumps diff directly with
   * tests/canary_diff.py. Reading a powered-down chip yields garbage or throws;
   * interpreting that is the caller's job. No-op where unsupported (default). */
  virtual void DumpChipState() {}
  /* The MAC's TX-DMA fault latch, for a caller that needs to know its
   * transmitter has stopped.
   *
   * This is not a statistic. The vendor driver treats ANY nonzero value as a
   * fatal TXDMA error and answers it with a MAC silent reset
   * (core/rtw_sreset.c, hal/rtl8822c/rtl8822c_ops.c in the rtl88x2cu tree);
   * once a bit here is set the part has stopped transmitting and will not
   * resume on its own. Measured on an RTL8812CU under a sustained downlink
   * load: 0x00040000, BIT_TXPKTBUF_REQ_ERR, latched while the receiver went
   * on working perfectly.
   *
   * NOT FOR THE SEND PATH. This is a register read over USB - see the
   * standing rule in CLAUDE.md that nothing reads a register per frame - so
   * poll it on a supervisory cadence, not per transmission.
   *
   * THROWS on a failed USB transfer, like every register read. A poller must
   * catch and skip that sample: on a Jaguar2 under a 4+4 Mbit/s load about
   * one control read a minute fails while the chip goes on working, and an
   * uncaught one killed an AP nine minutes into a soak.
   *
   * Returns 0 where unsupported, which is indistinguishable from healthy;
   * a caller that needs to tell those apart should ask the backend. */
  virtual uint32_t GetTxDmaStatus() { return 0; }
  /* The whole MAC register window, 0x0000..0x0FFF, printed in the SAME
   * format as the rtl88x2cu vendor driver's /proc/.../mac_reg_dump - one
   * line per 16 bytes, "0x%04x 0x%08x  0x%08x  0x%08x  0x%08x" - so a dump
   * from each can be diffed line by line. That is its whole purpose: the
   * vendor driver's AP on an RTL8812CU carried an 8 Mbit/s downlink for 20 s
   * with zero loss and zero TX faults, and this project's AP on the SAME
   * adapter faults after one traversal of its TX page ring. Somewhere in this
   * window is the difference. 1024 register reads over USB: diagnostic use
   * only. No-op where unsupported. */
  virtual void DumpMacRegisters() {}
  /* Read the chip's internal packet memory through the debug window
   * (REG_PKTBUF_DBG_CTRL + 0x8000..0x8FFF), a port of halmac read_buf_88xx.
   * `sel` 0 = TX FIFO, 1 = the LLT (the linked list that chains TX pages).
   * `offset` is in bytes from the start of that memory. Diagnostic: it
   * borrows a shared debug window, so never call it on the send path.
   * Throws on a failed USB transfer, like GetTxDmaStatus - catch it.
   * Returns false where unsupported. */
  virtual bool ReadPacketBuffer(int sel, uint32_t offset, uint8_t *out,
                                size_t n) {
    (void)sel; (void)offset; (void)out; (void)n;
    return false;
  }
  /* The MAC carrier-sense gate, one bit at a time.
   *
   * SetCcaMode is all-or-nothing, and on Jaguar1 and Jaguar3 it is two
   * gates: 0x520[14] primary CCA (defers to a decodable preamble) and
   * 0x520[15] EDCCA (defers to raw in-band energy). They answer different
   * questions, and the two families measured so far DISAGREE about which one
   * stops an injector — so a caller diagnosing a deferral has to tell them
   * apart, and one that needs a single gate should not have to turn off
   * both. CLAUDE.md summarises the on-air delivery figures and
   * tests/dis_cca_tx_onair.sh is the harness behind them;
   * tests/cca_gates_regcheck.sh is the register-level check that this
   * contract holds, not a delivery measurement.
   *
   * `true` means DISABLED, matching SetCcaMode's argument sense and the
   * register's own polarity (bit set = gate off). SetCcaMode is exactly
   * SetCcaGates(d, d) and writes the same bytes it always did; it stays the
   * portable call, and is all a backend without the split offers.
   *
   * CONTRACT, because both halves of this have bitten:
   *
   *  - POST-BRING-UP ONLY. Both calls return false before Init/InitWrite:
   *    0x520 is meaningless until the MAC is configured, so reading it would
   *    be a fabricated gate state and writing it would poke an uninitialised
   *    MAC. `false` therefore means EITHER "not ported on this backend" OR
   *    "not brought up yet"; a caller probing capability at construction
   *    cannot tell those apart and must re-ask after bring-up. On a refusal
   *    GetCcaGates leaves its out-parameters untouched.
   *
   *    SetCcaMode is NOT the same, and the difference is pre-existing rather
   *    than something the split introduced: it returns void, so a
   *    pre-bring-up call cannot report anything, and what it does with one
   *    is per-backend. The way to ask for a gate state from bring-up is the
   *    tuning.disable_cca config knob, which Init applies once the MAC is
   *    up.
   *
   *  - STICKINESS SURVIVES A RETUNE ON BOTH, BUT ONLY ONE OF THEM MEANS IT.
   *    Measured on an 8812AU and an 8822C, the gate state is intact after
   *    SetMonitorChannel AND after FastRetune, both within a band and across
   *    a 5 GHz/2.4 GHz change, at 0x520, 0x524 and Jaguar1's BB thresholds.
   *    The mechanisms are not equivalent: Jaguar3 records the pair and
   *    re-asserts it in SetMonitorChannel (its FastRetune fallback does not,
   *    and does not need to), while Jaguar1 records nothing and survives
   *    only because its channel path happens not to rewrite those registers.
   *    Do not build on the Jaguar1 case — re-read with GetCcaGates rather
   *    than assume. Bring-up IS a reset on Jaguar1: Init/InitWrite
   *    unconditionally re-run SetCcaMode(_cfg.tuning.disable_cca), so a
   *    re-Init puts the gates back to the configured default. */
  virtual bool SetCcaGates(bool primary_disabled, bool edcca_disabled) {
    (void)primary_disabled;
    (void)edcca_disabled;
    return false;
  }

  /* Current gate state, read back from the hardware rather than remembered.
   * Same contract as SetCcaGates above: post-bring-up only, false where the
   * split is unavailable, out-parameters untouched on a refusal. */
  virtual bool GetCcaGates(bool &primary_disabled, bool &edcca_disabled) {
    (void)primary_disabled;
    (void)edcca_disabled;
    return false;
  }
};

#endif /* IRTL_RADIO_H */
