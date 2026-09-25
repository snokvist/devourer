# Jaguar3 TX page ring — the beacon overwrite, the fix, and what is still open

**Status 2026-09-25, updated the same day: the mechanism is found and the fix
replaced.** `222bcf9` wrote the ring terminator directly
(`terminate_acq_ring`); that matched the vendor chip's end state but not how it
gets there. The GENERAL_INFO lead below was tested and ruled out, and the real
mechanism turned out to be a one-constant porting defect: devourer enabled only
the DMA bits of `REG_CR` before the LLT init, where halmac enables all eight.
The direct write is gone; the constant is fixed. Item 1 below is the record.

The investigation narrative, the ruled-out hypotheses and the corrected claims
live in `docs/station-mode-plan.md` (Phase 5). The durable per-chip facts live
in `src/jaguar3/CLAUDE.md`. This file does not repeat them.

## The defect in one paragraph

The TX FIFO is 2048 pages of 128 bytes, chained by the LLT. The auto-LLT init
links every page to the next, `0 -> 1 -> ... -> 2047`, and page
`rsvd_boundary` (1938) is where the beacon lives. On a correctly configured
chip the allocator wraps at the boundary and writes `LLT[1937] = 0` itself;
with `REG_CR` holding only the DMA bits at the LLT init, it did not - the data
ring ran on into the reserved region. Under sustained TX with a beacon armed, the data allocator is
eventually handed page 1938 and overwrites the beacon; the next TBTT reads a
data frame as a beacon descriptor and `TXDMA_STATUS` latches
`BIT_TXPKTBUF_REQ_ERR`. The chip transmits nothing more for the life of the
process. The rtl88x2cu vendor driver's chip, read live on the same adapter,
differs from ours in exactly one LLT entry: `LLT[1937] = 0`. The chip writes that
entry itself - when the MAC is configured the way the vendor configures it.

| RTL8812CU AP, MCS7, ch6 | before | direct LLT write (`222bcf9`) | `REG_CR` fix (current) |
|---|---|---|---|
| downlink goodput | 0.445 Mbit/s | 24.3 Mbit/s, ~1% loss | 29.6 Mbit/s, 1.2% loss (top rung offered) |
| our beacon under downlink load | 8% of idle | 100% | 100% |
| flood ping | ~21 rt/s, 78–89% loss | 357 rt/s, 1.1% loss | 305 rt/s, 5.2% loss; re-run 351 rt/s, 1.4% |
| injection with the beacon armed | fault at ~172–208 frames | 2001/2001 | 4000/4000 |
| `txdemo` 8000 frames, max duty | clean | clean | 8050/8050, `txdma_status` 0 |
| acceptance `all` | - | 20/21 (wpa2 at 6M) | 21/21, one run |

Single runs each; the flood pair shows the run-to-run spread is several
points, so the new fix's downlink figure is "at least as good", not a
measured gain. The 30-minute soak ran on the direct-write fix only.

## Open items

### 1. RESOLVED - how the vendor's chip gets `LLT[1937] = 0`

**Answer: its hardware writes it, dynamically, because the vendor enables the
whole MAC before the LLT init. devourer enabled only the DMA bits.**

halmac's `MAC_TRX_ENABLE` for the 8822C (and 8822E, and 8822B) is `0xFF`:
HCI TX/RX DMA, TX/RX DMA, PROTOCOL, SCHEDULE, MACTX, MACRX. `init_trx_cfg`
writes it to `REG_CR` just before `priority_queue_cfg` runs the auto-LLT init.
devourer's port had `MAC_TRX_ENABLE = 0x0F` - the four DMA bits only - and set
the rest later, after the LLT init had already run.

How it was established, each step with the control that makes it mean
something:

| step | result |
|---|---|
| **A/B, GENERAL_INFO** (both arms with the direct write disabled) | control (no H2C): fault at 208 frames, `LLT[1937]` stays `0x792`. With GENERAL_INFO + PHYDM_INFO: fault at 206 frames, `LLT[1937]` stays `0x792`. |
| were the H2C packets really delivered? | both built byte-identical to the vendor's 80-byte transfers (headless test against the usbmon bytes); the H2C queue's hardware write pointer AND the firmware's read pointer both advanced by 64 bytes - received and consumed. **GENERAL_INFO is not the mechanism.** |
| does the vendor host write the LLT? | no - its whole captured bring-up touches the packet-buffer window once, for the GENERAL_INFO poll of the H2C queue |
| the vendor chip, freshly bound, idle | `LLT[1936..1939] = 791 792 793 794` - **identical to ours**. The terminator is not there at init. |
| the vendor chip after 500 injected frames, monitor mode, **no AP, no beacon** | `LLT[1937] = 0`. The allocator wraps at the boundary on its own. |
| diff of the vendor's register writes vs ours, TRX enable through LLT init (usbmon, both captures) | exactly one difference: `REG_CR` `0xFF` vs `0x0F` |
| devourer with `REG_CR = 0xFF`, no direct write | 2001/2001 then 4000/4000 frames, zero faults; after the run `LLT[1937] = 0` - written by the hardware - and page 1938 still holds the beacon descriptor |

The GENERAL_INFO port (a header-only byte-exact builder, its selftest, the
H2C-packet send path with its own sequence counter, and the A/B switches) is
kept on the local branch `xp/j3-general-info`, not in the tree: it is not
needed for this, and sending it in bring-up would change firmware state under
every existing Jaguar3 validation for no measured benefit. The captured bytes
are below in case a future feature needs the packet path.

<details><summary>The captured GENERAL_INFO / PHYDM_INFO bytes</summary>

usbmon, vendor rtl88x2cu on an RTL8812CU, bulk-OUT endpoint `0x05`, 80 bytes
each (48-byte descriptor `TXPKTSIZE=32 QSEL=0x13`, checksum `0x1320`, then the
32-byte packet):

```
GENERAL_INFO  01 ff 0d 00 0c 00 00 00  00 00 38 00 ...   FW_TX_BOUNDARY 56, seq 0
PHYDM_INFO    01 ff 11 00 10 00 01 00  03 02 05 33 00 07 00 00 ...
              rfe 3, HALMAC_RF_2T2R, cut 5, rx|tx ant 3|3, ext_pa 0,
              package_type 7 (from its MAC-hidden report), seq 1
```

Both firmware blobs (8822C 9.0.17, 8822E) report H2C format version 15.
</details>

### 2. The 8822E is untested

`HalmacJaguar3MacInit` serves both Jaguar3 dies, and the 8822E's halmac
defines the same `MAC_TRX_ENABLE = 0xFF`, so the fix is the vendor's own value
there too - but no 8812EU/8822EU was on the bench. *To close:* the `beacons`
and `thru` cells of `tests/sta_d2d_onair.sh` with an 8822E as the AP (the
harness takes `AP_VID`/`AP_PID`/`AP_SYSFS`), plus `ap_wpa2` with
`DEVOURER_AP_INJECT=4000 DEVOURER_AP_PKTBUF=1`: zero faults and the LLT
entry before the 8822E's `rsvd_boundary` reading 0 at the end of run. (The
probe prints entries 1936..1939, the 8822C's neighbourhood; check the 8822E's
boundary first.)

### 3. Jaguar1 - checked, clear. Jaguar2 - carries the same defect, unverified

**Jaguar1 (RTL8812AU, on air 2026-09-25): no instance of either Jaguar3
defect.**

- *The page ring* cannot run into the beacon by construction: the 8812A/8821A
  LLT init is the old manual one (`HalModule::InitLLTTable8812A`), and it ends
  the data free list explicitly - `LLT[txpktbuf_bndy - 1] = 0xFF` - with the
  beacon pages above the boundary on a separate ring. (The 8814A uses the
  auto-LLT and is not covered by this; no 8814AU was on the bench.)
- *The queue mapping* is consistent: every frame carries QSEL `0x12` (MGT) on
  the first bulk-OUT endpoint, and Jaguar1's own priority init maps MGT to
  the HIGH queue that endpoint feeds. Data therefore competes with
  management for HIGH-queue pages - a throughput question, not a fault.
- *On air*, the 8812AU as the `ap_wpa2` AP (`AP_SYSFS=7-1 AP_PID=0x8812`),
  the MT7612U as station, ch6, MCS7: `beacons` **3/3**, our beacon 101% of
  idle under downlink load and straight back after it; the AP aired 4320
  frames with 0 send failures. `thru` downlink clean to **13.8 Mbit/s at
  0.28% loss**, saturating at ~12.5-12.8 Mbit/s above that - about half the
  Jaguar3 AP's ceiling, degrading smoothly, never collapsing.

**One finding that is NOT Jaguar1's: the uplink into this 8812AU loses a flat
~18-21% at every rate**, 1 to 30 Mbit/s offered, and MCS1 no better than
MCS7, so the `thru` cell fails its 5%-loss gate in that direction. It is this
adapter or its placement, established by elimination on one 1 Mbit/s rung
(1373 station frames aired, `ARQ=0`, so each airs exactly once):

| receiver of the station's frames | captured |
|---|---|
| the 8812AU as devourer AP | 81.5% |
| the 8812AU as a plain devourer `rxdemo` monitor, no TX | 84.0% |
| the same, with the Jaguar1 DIG watchdog on (IGI walked 0x1c -> 0x2a) | 77.3% |
| **the same 8812AU on the kernel's rtw88 driver, monitor** | **80.5%** |
| the 8812CU (vendor driver monitor / devourer AP), same moments | 94.8% / 99.0% |

Ruled out on the way: AP-mode TX/RX concurrency (plain monitor loses the same),
modulation margin (MCS1 no better; every frame that does arrive is strong -
RSSI ~83/72, EVM -54 dB), CRC-corrupted arrivals (only 2 of ~220 missing
frames show up as CRC failures with the address intact), and false-alarm
blinding by a low IGI (a hypothesis I tested and **retract**: DIG raised the IGI
and neither the garbage decode rate nor the capture moved). The same
8812AU is the bench witness that "goes deaf after a run"; together these say
the unit or its antenna position is marginal on this bench. Not pursued
further - it is not a devourer defect.

**Jaguar2 has the identical `REG_CR` defect, and it is not fixed.**
`src/jaguar2/HalmacJaguar2MacInit.cpp` defines the same DMA-only
`MAC_TRX_ENABLE = 0x0F` and writes it before its auto-LLT init, where the
vendor's 8822B halmac defines `0xFF`. No Jaguar2 adapter (8812BU/8822BU/
8811CU/8821CU) was on the bench, so it is flagged rather than changed. *To
close:* the one-constant fix, then `ap_wpa2` on a Jaguar2 AP with
`DEVOURER_AP_INJECT=4000` - zero faults, beacon page intact after - and the
`beacons` cell. Plain injection (the FPV path) is not expected to be
affected, as it was not on Jaguar3.

### 4. The `wpa2` acceptance cell is flaky at 6M on ch6

It fails its 16-of-20 threshold about one run in three (14/20 twice in six
runs). At MCS7 the same cell reads 19–20/20 every time, and 6M on ch6 was
already this lossy before the fix (27% at 10 pps, measured the same day), so
it is not a regression: the threshold was calibrated on ch36 (4–5% loss) and
the cell runs on ch6. Options, not chosen — they change what the gate means:
run the acceptance cells at MCS7; raise the sample to 50 pings with a
correspondingly derived threshold; or move `wpa2` to a cleaner channel.
With the `REG_CR` fix, one full `all` run passed 21/21 with this cell at
19/20 both ways - one run, so not evidence the flakiness is gone.

### 5. Why 6M legacy is less reliable than MCS7 on this bench

Unexplained, and backwards from physics — 6M is the more robust rate. Same
pair, same channel, same minutes: MCS7 one-way loss ~1%, 6M round-trip loss
10–30%. Candidates not yet separated: the legacy-rate TX descriptor fields on
either end (retry limit, `DISDATAFB`), a CCA/EDCA interaction specific to
longer frames on a busy band, or the MT7612U's legacy RX path. A `thru` ladder
at `TX_RATE=6M` against one at `MCS7`, one direction at a time, splits which
end.

### 6. No link-layer retransmission

Neither `ap_wpa2` nor `sta_client` arms `SetAckResponder`, so every lost frame
stays lost. `ARQ=1` in `tests/sta_d2d_onair.sh` arms the AP's responder and
has not yet been measured. With the downlink now working, it is measurable.

## The on-air regression set for any change here

```sh
sudo TX_RATE=MCS7 tests/sta_d2d_onair.sh beacons   # 3/3; ours 100% under downlink
sudo TX_RATE=MCS7 tests/sta_d2d_onair.sh thru      # both directions ~20 Mbit/s
sudo TX_RATE=MCS7 tests/sta_d2d_onair.sh flood     # 3/3; books close; ~350 rt/s
sudo tests/sta_d2d_onair.sh all                    # 21 checks (see item 4)
```

Plus a station-free stress that needs no association:
`DEVOURER_AP_INJECT=4000 DEVOURER_AP_PKTBUF=1` on `tests/ap_wpa2.cpp` — zero
send failures, no `TXDMA_STATUS` transition, and in the `end of run` probe
`LLT[1937]=0` (written by the hardware at the first wrap - it reads `0x792`
at init, correctly) with page 1938 still `59 00 30 85 ...`. And
`txdemo` with `DEVOURER_TX_FRAMES=8000 DEVOURER_TX_GAP_US=0`: `tx.stats`
carries `txdma_status`, which must stay 0.

## The answer-key technique

What broke this open was running the **vendor driver on the same adapter** and
reading its live state: `/proc/net/rtl88x2cu/<if>/mac_reg_dump` (whole MAC
window), `fifo_dump` (`echo "<sel> <hex off> <size>"`, sel 0 = TX FIFO,
4 = LLT), `read_reg`, plus usbmon for its descriptors. The vendor driver cannot
move its phy into a netns (`-95`), so run hostapd in the root namespace and
put the devourer station in the netns instead (`/tmp`-scratch
`vendor_ap.sh` did exactly this; worth landing as a harness if used again).
