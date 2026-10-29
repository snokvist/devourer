# p2plink hardware-trial brief

For the local session with the adapters plugged in. Nothing below the ctest
line has run on hardware; every design number is a hypothesis the bench
confirms or flips — write down which. Evidence base: the devourer docs cited
inline and the design evaluation this tree was planned from
(`p2p-opportunistic-ack-evaluation.md` plus its adversarial review), nothing else.

## 1. Status

**Implemented and headless-tested** (`ctest --test-dir build-p2p -L p2p`, six suites):
`wire` (header/dot11 codec), `fec` (RS codec vs the zfec construction, block
symbol sizing), `merge` (N-ear BlockMerger: first-copy-wins, gap rules,
anti-phantom, restart resync, SeqDedup), `link_ctl` (ladder, TRACK/BLIND/HOLD/
FLOOR, budget-leads timing, DaMachine), `budget` (airtime model, strict-priority
scheduler, video queue bound), `frame_shm` (venc ring round trip, Linux only).

**Never run on hardware**: all of `src/radio.cpp` (devourer glue: adapter
open, per-frame radiotap, ACK responder arming, tx.report sink, RX loops),
`p2p_tx`, `p2p_rx`, `p2p_feed`, and the tun bridge end to end.

**Design assumptions the hardware must confirm** — each with the measured
basis and the gap:

| # | assumption | measured basis | gap |
|---|---|---|---|
| A1 | the 8812EU closes a hardware-ACK loop as the *soliciting* TX (probe → ACK → `tx.report` ok) | 8812EU as **responder**: 98 % single-shot ACK on / 0 % off; the Jaguar3 soliciting row is the 8812CU (ACK 1.00, 0.24 mean retries); Jaguar2 soliciting is MARGINAL (12–91 % closure across identical runs) | 8812EU soliciting never measured; if it behaves like the Jaguar2 row the whole probe sensor is noise |
| A2 | 8812EU probe → MT7612U factory-MAC auto-ACK | 8812CU → MT7612U: 100 % ok, 0.01–0.45 mean retries, nothing armed (`MT_AUTO_RSP_CFG=0x13`) | 8812EU → MT7612U unmeasured; 8812EU injection → MT7612U monitor has no dedicated cell either |
| A3 | ears parked at 40 MHz decode the drone's 20 MHz frames on the primary with no retune, ~0 dB cost | `tests/rx80_narrow_tx_probe.sh` + `tests/wide_rx_penalty_sweep.sh`: 0 dB (≤1 dB) — 8822BU TX → 8814AU RX, 80-parked RX / 40 MHz frames | neither die, neither width pair is ours; standard 11n behaviour but unmeasured on 8812EU TX → 8812EU / MT7612U RX at 40/20 |
| A4 | `wide_tx_igi_floor` (default 0x35) is needed on *this* drone unit and its RX cost is bearable | one 8812EU never stalled, one 8812CU's edge sits in 0x2c..0x35 unbisected; cost **estimated** ~21 dB idle-channel RX sensitivity at 40 MHz (~1 dB/IGI step over the 0x20 DFS floor), **not measured** (`docs/experiments/8822c-ht40-tx-stall.md`) | per-unit edge unknown; the uplink (reports + probe ACKs) pays the cost, so a 21 dB hit would make BLIND/HOLD the normal state |
| A5 | an MT7612U ear receives at 40 MHz | MT7612U 40 MHz **TX** into a Realtek witness 242/242; `bw_mask` advertises 20/40/80 | MT7612U RX tuned at 40, and 20-in-40 decode, unmeasured |
| A6 | 1400 B frames are the right v1 size at MCS0 | PER model only: BER 1e-5 → 3836 B 26 %, 1500 B 11 %, 1000 B 7.7 %, 600 B 4.7 %; BER 1e-4 → 95/70/55/38 %; largest Jaguar3 payload in any devourer test is 1465 B | frame-length vs PER at MCS0 never measured on any die here, static or moving |

Also unmeasured and load-bearing: the 20 ms 8812EU send stall with
`no_cancel_multipkt` on (A7, C7) and blind-descent timing with a real encoder (C9).

## 2. Bench setup

**Adapters.** Drone: one 8812EU (`0bda:a81a`). Ground: an 8812EU and/or 1–2
MT7612U (`0e8d:7612`). Two adapters of the same part are not a replicate (a
whole "broken chip" posture once rested on one bad unit); record USB bus/port
per unit and keep the assignment fixed across cells.

**Kernel drivers.** The in-tree `rtw88` modules (`rtw88_8822eu` + `rtw88_usb`)
and `mt76x2u` (+ `mt76_usb`) auto-probe — and firmware-download into — every
matching dongle at every enumeration. `modprobe -r` does NOT survive a
re-enumeration, and the MT7612U re-enumerates at every bring-up (ZeroCD:
mass-storage `0e8d:2870` for ~1 s). Temp-blacklist instead:

```sh
printf 'blacklist rtw88_8822eu\nblacklist mt76x2u\ninstall rtw88_8822eu /bin/false\ninstall mt76x2u /bin/false\n' \
  | sudo tee /etc/modprobe.d/p2plink-bench.conf
sudo rmmod rtw88_8822eu mt76x2u 2>/dev/null; lsusb -t   # bus paths shuffle on every re-plug
```

Remove the file when done. `rmmod`/sysfs-unbind de-inits the chip (RF off),
so the first devourer open after it is a cold re-init. Bisect hardware
problems with a real VBUS power cycle (uhubctl on a hub with per-port power
switching, not an xhci root port; `authorized`-toggle "cold" leaves chip
state) — the chip retains state across a soft re-init, so a re-run is not cold.

**MT7612U firmware.** `mt7662_rom_patch.bin` + `mt7662.bin` ship
zstd-compressed in linux-firmware and are not embedded:

```sh
mkdir -p firmware && zstd -d /lib/firmware/mediatek/mt7662{,_rom_patch}.bin.zst -o firmware/
# then --mt7612u-fw ./firmware   (unset: /lib/firmware/mediatek, then ./firmware)
```

A run killed mid-transfer can leave the adapter unable to load firmware for
3–8 s; devourer retries the upload once. If every bulk-OUT NAKs after a crash,
re-plug (VBUS) before blaming the code.

**Near-field saturation.** Free-space loss is ~28 dB at 10 cm, ~57 dB at 3 m;
25 mW (+14 dBm) at 10 cm puts −14 dBm into the receiver — squarely in
compression. The tell is **EVM, not SNR** (SNR sat flat at 18 dB across a whole
sweep while EVM reversed at the knee). On the 8812EU use `DEVOURER_TX_PWR` /
the offset knob, a 30–40 dB SMA attenuator, or distance; never add power to a
link that reads SATURATED (`docs/bench-testing-near-field.md`). MT7612U ears
report no EVM and an RSSI-minus-noise "SNR" that is zeroed on quiet channels, so
judge saturation on the 8812EU ear.

**Qualify the ground before believing a delivery number.** A receiver whose
modulation cliff sits at the rate under test measures itself: same TX, minutes
apart, an internal-antenna 8822BU ran MCS7 at 2.9 % while an external-antenna
8814AU sat flat at ~80 %. Run `tests/ground_station_qualify.sh` against each
ear at the top rung you intend to test (R5 = MCS3/40, R6 = MCS5/40) and refuse
the pairing it refuses.

**±3 points is noise.** Ten identical back-to-back MCS7 probes with nothing
changed: sd 1.8, 5.7-point spread, while the MCS1 control held 98.0 ± 0.2.
Anything under ~4 points of delivery needs `tests/probe_repeatability.sh`
before it is a finding — several plausible "decay curves" sat inside that band.

## 3. Bring-up order and first light

The CLI contract both agents implement:

```
p2p_tx  --adapter VID:PID[@BUS-PORT] --channel N --bw 20|40 (40) --offset +|- (+)
        --net-id N --peer-mac XX:.. (REQUIRED, = p2p_rx's ack_target_mac)
        --ring NAME --tun NAME --tun-addr CIDR --bitrate-cmd 'cmd with %u kbps'
        --control-reserve-kbps N (300) --start-rung N (2) --fixed-rung N
        --no-tune20 --probe-ms N (100) --wide-igi-floor 0xNN --mt7612u-fw DIR
        --stats-shm NAME (/p2plink.tx) --no-ndjson --log-level L
p2p_rx  --adapter SPEC (repeatable; ear index = order) --ack-target IDX (0)
        --uplink IDX|none (0) --channel --bw --offset --net-id
        --out-udp HOST:PORT --out-shm NAME --tun NAME --tun-addr CIDR
        --report-hz N (10) --report-mcs N (0) --mt7612u-fw DIR
        --stats-shm NAME (/p2plink.rx) --no-ndjson --log-level
p2p_feed --ring NAME --synthetic --fps N --kbps N --idr-every N --enhance-ratio F
         | --file X.h265 [--loop] [--fps N]
```

Order, every time:

1. **Ground first.** MT7612U bring-up does not reliably complete while the
   channel is saturated (the MCU times out waiting for an arm that never
   becomes available — the `src/mt7612u/` subtree notes). Start `p2p_rx`, wait for
   `{"ev":"p2p.ready",...,"ack_target_mac":"..."}`. Check the printed MAC is
   unicast (I/G clear) and is not any other ear's permanent MAC.
2. **Drone at a fixed rung.** `p2p_tx ... --peer-mac <that MAC> --fixed-rung 2
   --ring /p2pbench` with `p2p_feed --ring /p2pbench --synthetic --fps 30
   --kbps 3000 --idr-every 30`. R2 is MCS2/20 — no 40 MHz frame, so the IGI
   floor is never armed in this step.
3. **Expected events**, in order: drone `p2p.ready`; ground `p2p.stats` with
   `uniq` rising at ~30 blocks × 8 symbols/s; drone `p2p.stats` with `ack8`
   reaching 8 within ~1 s of probes; `p2p.da` → `Ucast` after the 1 s hold
   (only with an uplink: the DaMachine needs TRACK, which needs reports).

**What "working" looks like** (near field, R2, two or three ears):
`uniq` rising monotonically; `diversity ≈ uniq × (ears − 1)` (every ear hears
every frame — any ear short of that is deaf or saturated); `fec_fail_permille`
0 and `loss_permille` ≈ 0; drone `ack8 == 8`, `retries_mean_centi` < 50,
`report_age_ms` < 200; `da_mode` Ucast; `tx_timeouts` 0; a `ping` through the
tun answers. One of those failing is the first cell's result, not a reason to
tune.

## 4. Bench cells, in order

Each cell: purpose · commands · what to grep · pass criterion · **what flips
if it fails**. Log every run (`| tee cell-CN-run-K.ndjson 2> cell-CN-run-K.err`).

### C1 — 8812EU soliciting TX vs 8812EU responder (assumption A1)

Purpose: the probe sensor exists at all on this drone die.
```sh
p2p_rx --adapter 0bda:a81a@<ground> --ack-target 0 --uplink 0 --channel 100 --bw 40 --net-id 7
p2p_tx --adapter 0bda:a81a@<drone> --peer-mac <ack_target_mac> --fixed-rung 2 --ring /p2pbench ...
```
Grep: drone `p2p.stats` `ack8`, `retries_mean_centi`, `probe_age_ms`; devourer
`tx.report` lines on stderr/stdout (`ok`, `retries`). Then kill `p2p_rx` and
read the same fields for the OFF shape. Compare with
`tests/ack_txreport_matrix.sh` (on: ok ~1 / retries ~0; off: ok ~0 / retries
pinned at the limit, 3 here) — run that harness with `CELLS` set to the 8812EU
if you want the retry *distribution*, which `p2p.stats` reduces to a mean.
Pass: ack8 ≥ 7 sustained with retries mean < 0.5 ON; ack8 → 0 with every
reported probe pinned OFF; report coverage (reported probes / sent probes)
≥ 0.8. **Flips if it fails:** the 8812EU cannot be the drone, or the probe
sensor is demoted to secondary and `rep_age` becomes primary (HOLD is then
unreachable and blind descent is the only failsafe).

### C2 — 8812EU → MT7612U ACK target (A2)

Same as C1 with `--adapter 0e8d:7612@<x> --ack-target 0 --uplink none`
(MT7612U-only ground). Additionally confirm the ear still decodes group frames
while ACKing: `uniq` must rise at the full feed rate during the run — the
auto-responder and the promiscuous monitor filter are supposed to coexist
(measured on an 8812CU stimulus; not on ours).
Pass: as C1, and `uniq` rate equal (±3 points) to the C1 ground's. **Flips:**
MT7612U-only grounds are dropped from v1; an 8812EU becomes mandatory as the
ACK target.

### C3 — 40 MHz-parked ground decodes 20 MHz and 40 MHz frames (A3, A5)

Ground: `--bw 40` with one 8812EU ear and one MT7612U ear (both `--ack-target
0`'s siblings). Drone: `--fixed-rung 3` (MCS3/20) for 60 s, then
`--fixed-rung 5` (MCS3/40, same MCS — isolates width) for 60 s, same
attenuation. Also run `--fixed-rung 5 --wide-igi-floor 0x35` explicitly if the
first 40 MHz frame does not air (see C4).
Grep: ground `p2p.stats` `uniq`, `diversity`, `adapters`, per-ear counts in
the ground's own per-adapter line; drone `tx_submitted`.
Pass: `adapters == 2` throughout; per-ear uniq within ±3 points of each other
and of `tx_submitted` in both widths; loss ≈ 0 near field. **Flips:** MT7612U
missing 20-in-40 frames → ground parks at 20 MHz, R4–R6 removed; missing
40 MHz frames → MT7612U ears are 20 MHz-only and R4+ needs an 8812EU ear.

### C4 — wide_tx_igi_floor edge and its uplink cost (A4)

Purpose: bisect *this* drone unit's stall edge, then measure what the floor
costs the uplink — the number the design estimated at ~21 dB and never
measured.
Edge: drone `--fixed-rung 5 --wide-igi-floor V` for V in 0x35, 0x30, 0x2c,
0x28, 0x24, 0x20, 0 on a quiet channel (the stall is a quiet-channel
phenomenon). The first 40 MHz frame must air: `tx_submitted` AND the ears'
`uniq` rising within 2 s; a stall is `tx_submitted` stuck or `tx_failed`
climbing with `uniq` flat. Record the lowest V that clears three cold starts;
`0` (never stalls) is a valid answer.
Cost: at matched attenuation (pick one where C1 gives ack8 ≈ 6–7, i.e. on the
edge) run `--fixed-rung 5` with the floor at your edge value and at 0x35, and
`--fixed-rung 3` (20 MHz frames; the floor stays armed because the tune is 40)
vs `--fixed-rung 3 --no-tune20` removed (default: retune to 20 after 3 s,
floor dropped). Grep drone `report_age_ms`, `ack8`, `retries_mean_centi`.
Pass: a documented edge value; cost in dB of attenuation between "ack8 ≥ 7"
with floor off and on. **Flips:** if the cost is ≥ ~10 dB, R4–R6 are only
usable with a strong uplink and the tune-to-20 after 3 s becomes 1 s; if the
unit never stalls, ship with `--wide-igi-floor 0`.

### C5 — blind descent and HOLD

Purpose: the failsafe timing (R−2 per 500 ms, floor from R6 in ~2.5 s) and
the ACK-alive HOLD exception. Drone adaptive (`--start-rung 5`, no
`--fixed-rung`), ground 8812EU ack_target + uplink, near field.
(a) Kill the uplink but keep the ACK: restart `p2p_rx` with `--uplink none`
(the ACK target stays). Expect within 500 ms a `p2p.rung` budget step to R3,
then a PHY step to R3 500 ms later, then **HOLD** (link_state HOLD, rung
stays) because `ack8 ≥ 6`. No further descent while probes are ACKed.
(b) Unplug the ACK/uplink adapter (or kill `p2p_rx`): expect budget R−2 at
+500 ms, PHY +500 ms, further R−2 steps every 500 ms, floor (R0) within
~2.5 s of the last report; `ack8` falls to 0 within 8 probes.
(c) Restore: first fresh report → TRACK; rung does NOT snap back (promotion
only after ≥10 fresh reports, ≥5 s dwell).
Grep: `p2p.rung` (one event per budget step and per PHY step, in that order),
`p2p.stats` `link_state`, `rung`, `report_age_ms`, `ack8`.
Pass: the timings within ±100 ms of the design; HOLD in (a), floor in (b), no
snap-back in (c). **Flips:** a descent that overshoots the floor time by
> 1 s, or a HOLD that never engages, means the probe reports arrive too late
or too sparsely for `ack8` — re-check C1 coverage before touching the
thresholds.

### C6 — DA machine against a 10 pps IP class, ground unplugged

Purpose: the fps-independence of the DA machine (the review's finding 3: a
count-based machine never left UNICAST against a dead peer at 10–20 pps).
Drone: adaptive, tun up; `ping -i 0.1 10.99.0.2` through the tun. Confirm
`p2p.da` Ucast first, then unplug the ground's ACK/uplink adapter.
Grep: `p2p.da` (expect `Bcast` within ~1 s: two pinned probes or ack8 ≤ 5,
then the 1 s min dwell), `tx_duty_permille` (must not jump — the storm this
prevents was 3.6× airings / 41× soliciting frames on the reference link; here
a dead IP frame costs at most 4 airings until the flip), `dropped_ip_pkts`.
Pass: Bcast within 1.5 s of unplug; duty stays within +50 ‰ of the pre-unplug
value; on re-plug ping resumes and `p2p.da` returns to Ucast only after
ack8 ≥ 7 for 1 s AND TRACK. **Flips:** a late or missing Bcast means the
pinned-probe trip is not firing — check that pinned reports (state 1) are
being fed, not dropped as unknown.

### C7 — 20 ms-stall soak (A7)

Purpose: the 8812EU's occasional 20 ms synchronous send stall on 5 GHz
(measured: send-time max 20.99 ms, p99 10 ms behind a 206–235 µs median,
`docs/stream-timing.md`) must not wedge TX with `no_cancel_multipkt` on (it is
hard-wired on in the drone's DeviceConfig — a cancelled multi-packet bulk-OUT
left an 8822EU TXDMA dead until re-init, `src/DeviceConfig.h`).
Drone `--fixed-rung 5` then `--fixed-rung 6`, 1400 B frames, feed at the
rung's budget, 30 min each.
Grep: drone `tx_submitted` (must keep rising at the feed rate), `tx_timeouts`
(chip FIFO full — expect some bursts, each one a local demote in adaptive
mode), `tx_failed`, `q_video_ms` (bounded at 60), `dropped_video_frames`.
Pass: `tx_submitted` never flat for > 2 s; no `tx_failed` run longer than one
stall; clean exit on SIGINT (`p2p.exit`) — a Stop() that hangs on a NAKed send
is the knob's documented cost and is a finding. A "without no_cancel_multipkt"
arm needs a flag the apps do not have; add one only to reproduce a wedge.
**Flips:** a wedge with the knob on means the stall is not the mechanism the
knob guards; TX then needs its own thread + progress watchdog + supervisor
restart before any flight.

### C8 — frame length vs PER at MCS0 on a moving platform (A6)

Purpose: the review's model says PER at equal BER scales ~linearly with
length (1500 B 11 % vs 600 B 4.7 % at 1e-5; 70 vs 38 % at 1e-4) and the
coherence time at 5.5 GHz / 20 m/s (~0.5–1.1 ms) is shorter than a 4.7 ms
MCS0 frame. Unmeasured on these dies. **v1 fixes the body at 1400 B and has
no frame-size knob**, so this cell cannot run as designed — log it as the
first follow-up (§5). Doable now: a walk test at `--fixed-rung 0` recording
`loss_permille`, `fec_fail_permille` and `rssi_best` per 100 ms — the 1400 B
baseline the later sweep is compared against.

### C9 — encoder in the loop

Purpose: budget-leads-PHY with a real encoder, and the video queue bound.
waybeam venc → its frame ring → `p2p_tx --ring <venc ring> --bitrate-cmd
'<venc bitrate set URL with %u>'`, ground as in C5, adaptive from
`--start-rung 4`. Demote by stepping attenuation up (sweep from the safe end;
the whole commanded range, not to the first wall).
Grep: `p2p.rung` budget step strictly before the PHY step (500 ms apart), the
`--bitrate-cmd` invocation (log it) between them; `p2p.stats` `q_video_ms`
≤ 60 throughout, `dropped_video_frames` small and only Enhance-layer when the
feed carries one, `video_budget_kbps` tracking the rung; ground `fec_fail_permille`.
Pass: no PHY step without a preceding budget step; `q_video_ms` never > 60
for more than one publish; after each demote the ground's `fec_fail_permille`
returns to 0 within 1 s. **Flips:** a PHY step that lands before the encoder
has come down (ground fec_fail spike lasting > 1 s) means the 500 ms lead is
too short for this encoder — measure its re-target latency and set the lead
to it, do not shorten the descent.

## 5. Known gaps / follow-ups

- **Frame size as a rung.** v1 fixes 1400 B (tun MTU fits one frame, MT7612U
  3836 B RX cap moot). The top rung pays ~20 % of peak for it; the floor would
  gain from 600–1000 B frames by the PER model above. Needs a knob, C8, then
  a ladder column.
- **Ground unicast IP only via an 8812EU uplink.** An MT7612U uplink is
  broadcast-only (75 fps unicast cliff) and its ACK is not
  backpressure-truthful (64-slot hand-off queue drops newest, so an ACKed frame
  can be undelivered) — with an MT7612U ACK target the IP class must stay
  broadcast(+repeat). Not enforced in code yet; enforce once C2 says which.
- **No encryption.** Open link by design; devourer's injection path encrypts
  nothing.
- **No channel migration.** Static per-boot channel; manual switch only.
  devourer's `src/chanmig/` is the ready protocol if it is ever added; keep
  the review's rule (switch only in TRACK with ack8 ≥ 7 and report age
  < 200 ms, final jump, no revert-to-home).
- **MT7612U-only ground caveats.** No backpressure (above); no FastRetune
  (789 ms per channel measured on a node, 526 ms on the devourer bench — a
  4-channel sweep is ~3 s dark); no EVM / real SNR / RX timestamps; bring-up
  unreliable on a saturated channel, so a ground reboot mid-flight may not
  come up until the drone's duty drops.
- **Not implemented: "no probe ACK for 30 s → duty 0.3".** The drone-side
  rule that would let a saturated MT7612U ground come back up. FLOOR today
  keeps the floor rung's full duty (0.60).
- **Encoder floor co-design.** R0's budget (~2.2 Mb/s by the PHY model with
  the 300 kb/s reserve; the reference link measured 20–40 % below the model)
  needs a resolution/fps profile chosen for bpp at that rate. The link only
  commands bitrate; the profile change is the encoder's.

## 6. Reporting back

Per cell, keep: both apps' NDJSON logs (`p2p.stats` at 10 Hz is the record:
`rung`, `link_state`, `da_mode`, `ack8`, `retries_mean_centi`,
`report_age_ms`, `loss_permille`, `fec_fail_permille`, `uniq`, `diversity`,
`adapters`, `tx_submitted`, `tx_failed`, `tx_timeouts`, `q_video_ms`,
`dropped_video_frames`), the `p2p.rung` / `p2p.da` / `p2p.tune` lines,
devourer's `tx.report` lines and stderr, adapter identities (bus/port,
permanent MAC per unit), attenuation / distance, channel, and
`lsmod | grep -E 'rtw88|mt76'` (must be empty). Put each pass/fail next to the
assumption it tests (A1–A7) and name the design decision it confirmed or flipped.

Two rules, both borrowed and both paid for already:

- **A result that cannot be used must not report success.** A cell that
  decoded nothing is NONE, not PASS; a saturated near-field run is not a
  delivery number; `report coverage < 0.8` makes `ack8` unusable, so C1
  without coverage is INCONCLUSIVE. Say so in the log line.
- **Two adapters of the same part are not a replicate.** Per-unit edges
  (C4), per-unit ACK behaviour and per-unit stalls are all on record. Record
  which unit, and repeat on the second unit before writing "the 8812EU does
  X".

And the house rule of the tree this lives in: a favourable number goes next to
its adversarial counterpart, every time.
