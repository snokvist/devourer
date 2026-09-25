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
measured gain.

**Bidirectional soak on the fix, 5 GHz (ch36, MCS7, 4 Mbit/s each way at
once):** 8812CU AP 30 min and 8812BU AP 15 min, both **5/5** - one
association, zero MIC failures, both ledgers close (~643k and ~322k frames
each way), no quarter-on-quarter degradation. Counterparts: downlink loss
averaged 3.3% (8812CU, worst chunk 5.0%) and 3.1% (8812BU, worst 3.9%) with
no ARQ; AP resident memory grew 16 kB in 30 min on the 8812CU but **532 kB
in 15 min on the 8812BU** - one run, so leak vs warm-up is not separated.
The first 8812BU soak did NOT survive: see Jaguar2 below.

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

### 2. RESOLVED - the 8822E had the defect, and the fix removes it

Measured 2026-09-25 on an 8812EU (1-1, `0bda:a81a`). Same `rsvd_boundary`
(1938) as the 8822C.

| 8812EU | result |
|---|---|
| control, `MAC_TRX_ENABLE` temporarily back to `0x0F`, 4000 frames with the beacon armed | fault at 172 frames, page 1938 = `5a 5a ...`, `LLT[1937]` never terminated |
| `0xFF` | 4000/4000, no fault, end-of-run `LLT[1937] = 0`, beacon page intact |
| `txdemo` 8000 frames, max duty | 8050/8050, `txdma_status` 0 |
| as AP, MT7612U station, **ch36** | `beacons` 3/3 (ours 103% of idle under downlink); `thru` 2/2 - up 19.9 Mbit/s at 0.27%, down 29.8 Mbit/s at 0.81% |
| as AP, **ch6** | the station never completes the four-way - the documented 2.4 GHz TX limitation of this module (`docs/8822e-quirks.md`, "2.4 GHz TX: undecodable", kernel parity), not this fix |

One observation against that quirk entry, not pursued: on ch6 the MT7612U
decoded about half of the 8812EU's beacons (~20/s of 39/s aired), where the
quirk records no receiver decoding any 2.4 GHz TX from this module.

### 3. Jaguar1 - checked, clear. Jaguar2 - had the same defect, fixed

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

**Jaguar2 had the identical `REG_CR` defect - fixed and measured 2026-09-25**
on an 8812BU (6-1, `0bda:b812`). `src/jaguar2/HalmacJaguar2MacInit.cpp`
had the same DMA-only `MAC_TRX_ENABLE = 0x0F`; halmac's is `0xFF` for both
the 8822B and the 8821C. Same `rsvd_boundary`, 1938.

| 8812BU | result |
|---|---|
| unchanged code (`0x0F`), 4000 frames with the beacon armed | fault at 358 frames: page 1938 overwritten with `5a 5a ...`, `TXDMA_STATUS` 0 -> `0x10` -> `0x15`, then every bulk-OUT times out |
| `0xFF` | 4000/4000, no fault, end-of-run `LLT[1937] = 0`, beacon page intact |
| `txdemo` 8000 frames, max duty | 8042/8042, `txdma_status` 0 |
| as AP, MT7612U station, ch6 | `beacons` 3/3 (ours 100% of idle under downlink, 4317 aired, 0 failed) |
| as AP, **ch36** | `thru` 2/2 - up 19.9 Mbit/s at 0.16%, down 29.6 Mbit/s at 1.34% |
| as AP, **ch6** | `thru` FAILS its gate: downlink carries up to 28.4 Mbit/s but at a flat ~5.5-6% loss; uplink loses 40-47% at every rate (an earlier run: 31% falling to 2.5%) |

**The ch6 losses are the adapter or its placement, not Jaguar2** - split the
same day with the 8812CU back on the bench as a known-good ch6 AP and
witness. Uplink, the station's frames at the same moments: the 8812CU AP got
99.2%; the 8812BU on the kernel's **rtw88** got 74.9%; the 8812BU on
**devourer** got 89.0% (deduplicated by CCMP PN - Jaguar2's parser leaves
`rx.frame` `seq` at 0). devourer's receive path does no worse than the kernel
on this unit. Downlink, from the 8812BU AP: 5365 data frames submitted, 5197
(96.9%) seen on air by the 8812CU witness, 4986 (92.9%) decrypted by the
station - the loss is mostly on the receiving side of that path; Jaguar2 TX
accounts for at most ~3%, indistinguishable from the witness's own misses.
The 8812BU-as-AP uplink losses (31-47%) exceed its monitor-mode 11%; those
were separate runs on a busy 2.4 GHz channel and AP mode is clean on ch36,
so the excess is recorded, not explained.

**A second Jaguar2 defect found on the way, fixed:** the first `thru` run
killed the AP. Its DIG thread (`RtlJaguar2Device::StartRxLoop`) called
`dig_step()` every 100 ms with no exception handling, and a USB control read
(`rtw_read(0c50)`, the IGI) threw `iostream error` under a 14-20 Mbit/s
uplink - `std::terminate`, core dump. The file already documented that such
reads "race the async bulk-IN and throw under load" and guarded the CFO
tracker for it; DIG and the thermal-track thread were not guarded. Both now
skip and count the tick. It fired about once per ladder afterwards - a
recurring event, not a one-off. **And it is not only DIG's problem:** the
first 15-minute 8812BU soak died at minute 9 the same way, through the
`GetTxDmaStatus` read this session added to Jaguar2 - `ap_wpa2`'s TX-DMA
watchdog polls it every 100 ms and did not catch. The ap log shows 12
isolated read failures over those 9 minutes with bulk traffic succeeding
between every one: the chip was healthy, the reads just fail about once a
minute under a 4+4 Mbit/s load. The watchdog, the PKTBUF probe and
`txdemo`'s `tx.stats` now catch and skip the sample (`tx.stats` reports
`txdma_read_failed` instead of a 0 that would read as healthy), and the
`IRtlRadio` contract says so. The rerun passed 5/5 with 17 failed reads
absorbed. Jaguar2's `ReadPacketBuffer` and
`GetTxDmaStatus` were ported from Jaguar3 (read-only; the 88xx common
`read_buf` addressing) to make the station-free probe work on this die.

Not covered: the 8821C (USB) and the PCIe 8821CE share the constant and
were not on the bench.

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

### 6. RESOLVED - retransmission: the AP's retry limit, not the responder

Measured 2026-09-25 with the new `AP_RETRY` harness knob:

| arm (8812CU AP, MT7612U station, ch36, MCS7) | uplink loss, 1-20 Mbit/s | downlink loss, 1-30 Mbit/s | station replays rejected |
|---|---|---|---|
| A0 no responder, no AP retries (every earlier figure) | 0.14-0.30% | 1.26-2.27% | 0 |
| A1 `ARQ=1` (AP ACK responder) only | 0.19-0.41% | 0.75-2.16% | 0 |
| A2 `AP_RETRY=7` only | 0.15-0.45% | **0.00% on every rung** | 115 |
| A3 both | 0.09-0.45% | **0.00% on every rung** | 114 |

One ladder per arm. What it establishes:

- **The downlink loss was single-shot frames.** The AP transmits with the
  library's default retry limit, 0 (`DEVOURER_TX_RETRY_LIMIT` - right for the
  FPV link, where FEC carries reliability), so every downlink frame aired
  once and a frame the station missed stayed lost. With a retry limit of 7
  the loss is zero up to 30 Mbit/s (29.998 delivered). The ~115 replays the
  station rejects are the fingerprint of it working: frames whose ACK was
  lost, retried, and correctly dropped as copies - nothing delivered twice.
- **The ACK responder (`ARQ=1`) changes nothing here**, because the AP
  already ACKs the station: over 321k soak frames the AP rejected zero
  replays, and the station's MT7612U requests an ACK on every frame with a
  15-deep retry (`MT_TX_RETRY_CFG` 0x47f01f0f) - unACKed frames would have
  aired repeatedly and shown up as replays. The harness comment that
  predicted `ARQ=1` would improve the uplink was wrong and is corrected.
- The uplink's residual 0.1-0.4% is not the air's retry budget; it is not
  explained yet. The 30 Mbit/s uplink rung loses 21-25% in every arm - the
  station dropping 8-10k frames from its own queue, a send-path ceiling.

`AP_RETRY` is a harness knob, unset by default so every recorded figure stays
reproducible.

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
at init, correctly) with page 1938 still `59 00 30 85 ...`. The probe reads
around page 1938; `DEVOURER_AP_PKTBUF_BNDY=N` points it at another die's
boundary (all three measured dies - 8822C, 8822E, 8822B - use 1938). It
works on Jaguar2 and Jaguar3, the backends with `ReadPacketBuffer`. And
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
