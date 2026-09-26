# Jaguar3 TX page ring — the beacon overwrite, the fix, and what is still open

**Status 2026-09-25, updated the same day: the mechanism is found and the fix
replaced.** `222bcf9` wrote the ring terminator directly
(`terminate_acq_ring`); that matched the vendor chip's end state but not how it
gets there. The GENERAL_INFO lead below was tested and ruled out, and the real
mechanism turned out to be a one-constant porting defect: devourer enabled only
the DMA bits of `REG_CR` before the LLT init, where halmac enables all eight -
and a bit bisection names the one that matters, PROTOCOL_EN (bit 4). The
direct write is gone; the constant is fixed. Item 1 below is the record. The
Phase 5 close-out review corrected several claims in this file; each
correction is marked where it was made.

The investigation narrative, the ruled-out hypotheses and the corrected claims
live in `docs/station-mode-plan.md` (Phase 5). The durable per-chip facts live
in `src/jaguar3/CLAUDE.md`. This file does not repeat them.

## The defect in one paragraph

The TX FIFO is 2048 pages of 128 bytes, chained by the LLT. The auto-LLT init
links every page to the next, `0 -> 1 -> ... -> 2047`, and page
`rsvd_boundary` (1938) is where the beacon lives. On a correctly configured
chip the hardware terminates the data ring at the boundary itself -
`LLT[1937]` reads `0x792` at init and `0` by the end of any run that has gone
past one traversal (the moment it changes was not observed); with `REG_CR`
lacking PROTOCOL_EN at the LLT init, it did not - the data ring ran on into
the reserved region. Under sustained TX with a beacon armed, the data allocator is
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
measured gain. (`txdemo`'s `submitted` is the transport's count of every
bulk-OUT, bring-up's included; the demo's own frames stop at exactly 8000,
and the extra 50 (8822C/E) / 42 (8822B) fit each die's firmware-download
chunk count - fits, not verified.)

**Bidirectional soak on the fix, 5 GHz (ch36, MCS7, 4 Mbit/s each way at
once):** 8812CU AP 30 min and 8812BU AP 15 min, both **5/5** - one
association, zero MIC failures, both ledgers close (~643k and ~322k frames
each way), no quarter-on-quarter degradation beyond the grader's 20%
allowance (8812CU up 3.984 -> 3.985, down 3.862 -> 3.860 Mbit/s; 8812BU up
3.996 -> 3.996, down 3.919 -> 3.856). Counterparts: downlink loss
averaged 3.3% (8812CU, worst chunk 5.0%) and 3.1% (8812BU, worst 3.9%) with
no ARQ; AP resident memory grew 16 kB in 30 min on the 8812CU but **532 kB
in 15 min on the 8812BU** - one run, so leak vs warm-up is not separated.
The first 8812BU soak did NOT survive: see Jaguar2 below.

## Open items

### 1. RESOLVED - how the vendor's chip gets `LLT[1937] = 0`

**Answer: its hardware writes it, because the vendor enables the whole MAC -
PROTOCOL_EN in particular - before the LLT init. devourer enabled only the
DMA bits.**

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
| **which bit** (asked by the close-out review; 8812CU, ch36, 4000 frames each) | `0x1F` (DMA + PROTOCOL): **4002/4002, clean, terminator written**. `0x2F` (DMA + SCHEDULE): fault at 212. `0xCF` (DMA + MACTX + MACRX): fault at 175. PROTOCOL_EN is the bit, on the 8822C; the 8822E and 8822B were fixed with the vendor's full `0xFF` and not bisected. |

The usbmon diff covered the window from the TRX enable to the LLT init. The
bisection is what places the requirement AT the LLT init: the later full
`REG_CR` write (`0x06FF`, which includes PROTOCOL_EN) comes after the PHY
tables, and the `0x0F` arms still faulted.

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
| at init, before the beacon (both arms) | `LLT[1936..1939] = 791 792 793 794`, the same as the 8822C |
| control, `MAC_TRX_ENABLE` temporarily back to `0x0F`, 4000 frames with the beacon armed | fault at 172 frames, page 1938 = `5a 5a ...`, `LLT[1937]` never terminated |
| `0xFF` | 4000/4000, no fault, end-of-run `LLT[1937] = 0`, beacon page intact |
| `txdemo` 8000 frames, max duty | 8050/8050, `txdma_status` 0 |
| as AP, MT7612U station, **ch36** | `beacons` 3/3 (ours 103% of idle under downlink); `thru` 2/2 - up 19.9 Mbit/s at 0.27%, down 29.8 Mbit/s at 0.81% |
| as AP, **ch6** | the station never completes the four-way. UNATTRIBUTED: consistent with the documented 2.4 GHz TX limitation of this module (`docs/8822e-quirks.md`), but ch6 was never tried before the fix, so "not this fix" is untested |

And an observation that contradicts that quirk entry (which records no
receiver decoding any 2.4 GHz TX from this module): on ch6 the MT7612U
decoded about half of the 8812EU's beacons (~20/s of 39/s aired). One run;
the quirk entry is annotated.

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

**The uplink into this 8812AU loses a flat ~18-21% at every rate**, 1 to 30
Mbit/s offered, and MCS1 no better than MCS7, so the `thru` cell fails its
5%-loss gate in that direction.

**CORRECTED TWICE.** First, by the close-out review: this section had said
"it is not a devourer defect" because "each frame airs exactly once". The
review answered that the MT7612U retries unacknowledged unicast 15 deep, and
since the witness saw zero station retries, the AP must have ACKed frames it
then dropped - "ACKed-but-undelivered". **That correction is itself
RETRACTED (2026-09-26):** the station sent every frame with the radiotap
NOACK flag (`build_stream_radiotap`'s default), which on the MT7612U clears
the TXWI ACK request - the station never waited for an ACK and never
retried. Zero retries was by request and proves nothing about ACKs
(`src/mt7612u/CLAUDE.md`). So "each airs exactly once" was TRUE, for a reason
nobody had written down. The Jaguar1 AP-mode ~18% is single-shot loss:
frames the 8812AU did not decode. Whether that is the unit/placement (as the
passive comparison below suggests) or devourer's Jaguar1 receive path is not
separated; with the station now able to retry (`STA_ACK=1`) it would be
largely masked either way. Not re-measured - no 8812AU on the bench.

What the table below does still show, for PASSIVE reception only (where the
station's frames are ACKed by another AP and single-shot to this receiver):
devourer's monitor RX on this unit is no worse than the kernel's. Evidence
from one 1 Mbit/s rung per receiver, separate runs:

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
and neither the garbage decode rate nor the capture moved). For passive RX
the unit (the bench witness that "goes deaf after a run") or its position
remains the leading explanation; for AP mode it is not, per the correction
above.

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
| as AP, **ch6** | `thru` FAILS its gate: downlink carries up to 28.4 Mbit/s but at a flat ~5.5-6% loss; uplink 40-47% at every rate in one run, 31% falling to 2.5% with rate in another |
| as AP, **ch6**, re-run 2026-09-26 with `AP_RETRY=3 STA_ACK=1 STA_RETRY=5` | `thru` **PASSES** 2/2: uplink 0.00% to 20 Mbit/s; downlink 0.67-3.10% per rung (30 Mbit/s at 0.67%). Same-session control, the 8812CU as AP, same station and channel: downlink 0.12-0.34%. One run each |

**The ch6 losses are UNATTRIBUTED** (corrected by the close-out review: this
paragraph first said "the adapter or its placement, not Jaguar2"). What the
split with the 8812CU back on the bench actually shows:

- *Passive RX only*, one 1 Mbit/s rung per receiver, **separate runs**: the
  8812BU on the kernel's rtw88 captured 74.9% (deduplicated by unique
  802.11 sequence number), on devourer 89.0% (by CCMP PN - Jaguar2's parser
  leaves `rx.frame` `seq` at 0), while the 8812CU AP of those runs got
  99.2%. devourer's passive RX is no worse than the kernel's on this unit.
- *AP-mode uplink* (2.5-47% across two runs) is worse than that passive
  figure. (The close-out review read this as the Jaguar1 "ACKed-but-
  undelivered" shape; that inference is retracted - see item 3: the station
  sent NOACK, so there was no retry to make AP mode better.) Single-shot loss
  on a busy 2.4 GHz channel; unattributed, not re-run with `STA_ACK=1`.
- *Downlink*: 5365 data frames submitted, 5197 (96.9%) seen on air by the
  8812CU witness, 4986 (92.9%) decrypted by the station. The witness's own
  passive miss rate was not measured, and the same station decoded the
  8812CU AP's ch6 downlink at ~98.8%, so a transmitter-side share (RF/EVM
  of this 8812BU, or its programmed TX power) is not excluded.

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

### 5. Why 6M is less reliable than MCS7 - split on 5 GHz; the "defect class" it seemed to find was the station's NOACK

Measured 2026-09-25, ch36, 8812CU AP, single-shot (`AP_RETRY` unset), one
ladder per arm (500-4000 kbit/s; each arm's saturated top rung excluded):

| arm | airtime / frame | uplink loss | downlink loss |
|---|---|---|---|
| MCS7, 1400 B (from item 6's A0) | ~0.2 ms | 0.14-0.30% | 1.26-2.27% |
| MCS0, 1400 B | ~1.8 ms | 0.07-0.15% | 2.99-4.52% |
| 6M, 1400 B | ~1.9 ms | 0.78-4.22% | 3.66-5.75% |
| 6M, 350 B | ~0.5 ms | 0.69-1.37% | 2.00-3.21% |

- **Downlink** (single-shot, no AP retries): loss rises with frame airtime
  but does not scale with it - a floor of roughly 1.5-2% plus an
  airtime-dependent part. Exposure plus a fixed per-frame miss; AP retries
  (item 6) remove both.
- **Uplink** was NOT retry-backed, though every account here said it was.
  The first reading: 6M worse than MCS0 at the same airtime, and a witness
  showing zero station retries while the AP delivered 99.46%, taken as the
  8812CU AP ACKing frames and then dropping them. **RETRACTED 2026-09-26.**
  The station sends every frame radiotap-NOACK, so it never retries; zero
  retries meant nothing. What the follow-up measured (ch36, 6M, 2 Mbit/s,
  30 s each, one run per arm):

  | arm | station frames | lost at the AP | of those on the air | not on the air |
  |---|---|---|---|---|
  | vendor rtl88x2cu AP (open; per-datagram by UDP sequence) | 5358 | 20 | 1 | 19 |
  | devourer AP (WPA2; per-frame by CCMP PN, AP PN log) | 5386 | 55 | 22 | 33 |
  | **devourer AP, station requesting ACKs (`STA_ACK=1`)** | 5386 | **0** | 0 | 0 |

  The instruments behind it: an AP-side PN log (`DEVOURER_AP_PN_LOG`),
  `ap_wpa2`'s new `rx path:` ledger line (frames reaching the callback
  before any filter), the Jaguar3 `RXDMA_STATUS` overflow flag in
  `DumpChipState` (clear at the end of the run - no RX FIFO overflow), the
  RX ring telemetry (never starved), and a witness checked against a known
  beacon each run. With ACKs requested, 63 retry-bit frames and 33 repeated
  PNs appear on the air, and every frame arrives. Frames "not on the air"
  in the NOACK arms are not explained - the witness's own miss rate was
  0.5-1.7% - but they vanish with retries on, so they are not a loss the link
  has to carry. The vendor/devourer difference in "on the air, lost" (1 vs
  22) is one run each and was not pursued once the NOACK cause was found.

### 6. RESOLVED - retransmission: the AP's retry limit, not the responder

Measured 2026-09-25 with the new `AP_RETRY` harness knob:

| arm (8812CU AP, MT7612U station, ch36, MCS7) | uplink loss, 1-20 Mbit/s | downlink loss, 1-30 Mbit/s | station replays rejected |
|---|---|---|---|
| A0 no responder, no AP retries (every earlier figure) | 0.14-0.30% | 1.26-2.27% | 0 |
| A1 `ARQ=1` (AP ACK responder) only | 0.19-0.41% | 0.75-2.16% | 0 |
| A2 `AP_RETRY=7` only | 0.15-0.45% | **0.00% on every rung** | 115 |
| A3 both | 0.09-0.45% | **0.00% on every rung** | 114 |

One ladder per arm, ch36 only - the ch6 conditions the old "27%" came from
are untested with `AP_RETRY`, and the library default (and the harness
default) is still retry limit 0, so a default d2d downlink still has no
retransmission. What it establishes:

- **The downlink loss was single-shot frames.** The AP transmits with the
  library's default retry limit, 0 (`DEVOURER_TX_RETRY_LIMIT` - right for the
  FPV link, where FEC carries reliability), so every downlink frame aired
  once and a frame the station missed stayed lost. With a retry limit of 7
  the loss is zero up to 30 Mbit/s (29.998 delivered). The ~115 replays the
  station rejects are the fingerprint of it working: frames whose ACK was
  lost, retried, and correctly dropped as copies - nothing delivered twice.
- **The ACK responder (`ARQ=1`) changes nothing here - because the station
  never asked for an ACK.** It sent every frame radiotap-NOACK, so whether
  the AP ACKs could not matter. (Two earlier explanations recorded here were
  wrong: "the AP already ACKs the station", argued from zero witness retries
  and an airtime bound, both of which assume the station was waiting for
  ACKs; it was not.) `StartBeacon` does program the same registers as
  `SetAckResponder` (MACID + net_type=AP), and the `STA_ACK=1` run shows the
  AP answering once asked. The harness comment that predicted `ARQ=1` would
  improve the uplink was wrong and is corrected.
- **The uplink's half is `STA_ACK=1`** (item 5): with it, a 6M uplink went
  to 0.00% loss. The 30 Mbit/s uplink rung loses 21-25% in every arm - the
  station dropping 8-10k frames from its own queue, a send-path ceiling.

**Both halves on (`STA_ACK=1 AP_RETRY=7`), ch36, one ladder per rate,
2026-09-26:**

| rate | uplink loss | downlink loss |
|---|---|---|
| MCS7, 1-20 Mbit/s up / 1-30 down | **0.00% on every rung** | **0.00%** on every rung but the first (6.64% at 1 Mbit/s, not reproduced in two later runs of that rung) |
| 6M, 0.5-4 Mbit/s | **0.00%** | **0.00%** |

**How many AP retries** (asked on review: 7 is a lot). Downlink-only
ladders, MCS7, ch36, `STA_ACK=1`, one run each: `AP_RETRY=3` 0.00% on every
rung to 30 Mbit/s except 0.01% (about one datagram in ~115k) at 20 Mbit/s;
`AP_RETRY=5` 0.00% everywhere; 7, 0.00% (above). **The harness now defaults
to `AP_RETRY=3` and `STA_ACK=1`** (2026-09-26) - the cheapest limit that
measured clean, on a strong, quiet link; a weaker one may want 5. `=0`
reproduces the older single-shot figures. With both on, `all` reads 21/21
with every ping cell 20/20 (the flaky `wpa2`-at-6M cell included, one run),
and a 4-minute bidirectional soak 0.00% both ways (one downlink chunk
0.01%).

**The station's retry limit** is the same knob on the other end
(`DEVOURER_TX_RETRY_LIMIT`, harness `STA_RETRY`, default 5). On the MT7612U
it is a global MAC register the initvals leave at 15; the backend now writes
it (only when the caller chose a value) and reads it back. Verified on the
chip's own TX status (`mt7612uprobe txs`, ch36): an unacknowledged frame
settles at 6 attempts with 5, 16 with the default. With it, the MCS7
uplink ladder read 0.00% to 20 Mbit/s and `all` 21/21.

**Duplicate detection** came with it. With both ends retrying, a lost ACK
delivers a frame twice, and the second copy reached the CCMP replay check -
the `wpa2` cell's `replays=0` ledger check failed on it (2 on the AP). Both
ends now run 802.11 duplicate detection (Retry bit + Sequence Control per
TID, `sta::DupDetector`, before decrypt), counted as `duplicates dropped`;
in that soak 257 at the station and 13 at the AP, with `replays rejected`
back to 0 at both.

So the 6M-vs-MCS7 difference (item 5) was never a property of the rate: it
was single-shot loss, larger for longer frames, and it disappears once
either end can retry. The 30 Mbit/s uplink rung still loses ~24% - the
station's own send-path ceiling (~22.7 Mbit/s), a separate question.

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
`LLT[1937]=0` (written by the hardware during the run - it reads `0x792`
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
