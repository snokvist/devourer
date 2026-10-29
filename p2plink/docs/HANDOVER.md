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
sweep while EVM reversed at the knee). On the 8812EU use `--txpwr-offset-qdb N`
(both apps; quarter dB vs the calibrated table — the library reads no
environment, so `DEVOURER_TX_PWR` is inert here), a 30–40 dB SMA attenuator,
or distance; never add power to a
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
        --no-tune20 --probe-ms N (100) --wide-igi-floor 0xNN
        --txpwr-offset-qdb N --mt7612u-fw DIR
        --stats-shm NAME (/p2plink.tx) --no-ndjson --log-level L (warn)
p2p_rx  --adapter SPEC (repeatable; ear index = order) --ack-target IDX (0)
        --uplink IDX|none (0) --channel --bw --offset --net-id
        --out-udp HOST:PORT --out-shm NAME --tun NAME --tun-addr CIDR
        --report-hz N (10) --report-mcs N (0) --txpwr-offset-qdb N (uplink)
        --mt7612u-fw DIR
        --stats-shm NAME (/p2plink.rx) --no-ndjson --log-level L (warn)
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
(the ACK target stays) — or, on a one-adapter ground, SIGUSR1 to `p2p_rx`
(mutes reports, keeps the adapter and its ACK up). Expect **HOLD** at the
report timeout (~500 ms) with the rung unchanged — no initial R−2 step: the
probes are ACKed, so the forward link is fine (DESIGN.md, decided after C5).
No descent while probes are ACKed.
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
- **Encoder floor co-design.** R0's budget (~2.0 Mb/s by the airtime model with
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

## Results

Bench host: one Linux x86_64 box, both ends local (ground tun in its own
netns for IP checks). Logs: `~/dev/snokvist/p2plink-bench/logs/<date>/`
(outside the repo; `<cell>.{rx,tx}.ndjson|.err|.runner`), produced by
`run_cell.sh` / `run_ping.sh` beside them. Kernel modules: `8812eu`,
`rtw88_8822eu`, `mt76x2u` temp-blacklisted (`/etc/modprobe.d/p2plink-bench.conf`),
`lsmod | grep -E 'rtw88|mt76x2|8812'` empty for every run below.

Units (fixed for every cell):

| role | part | USB | bus path | serial | permanent MAC |
|---|---|---|---|---|---|
| drone | RTL8812EU `0bda:a81a` (RTL8822E) | 480M (USB2) | 1-1 | 123456 | 84:fc:14:50:bc:de |
| ground ear 0 (ack target + uplink) | MT7612U `0e8d:7612` | 5000M | 8-1 | 000000000 | 40:a5:ef:5a:32:f8 |

**Only one 8812EU is on this bench.** C1 (8812EU responder) and the 8812EU
ear of C3 cannot run until the second unit is available; every cell below is
the 8812EU → MT7612U pairing. No control adapter and no second receiver
either, so no delivery figure here is cross-checked by an independent witness.

### First light — 2026-10-29, ch 100 HT40+, drone tuned 20 MHz (R2), no attenuator

Distance not measured (same desk); drone `--txpwr-offset-qdb -40` (−10 dB);
ground RSSI −31..−47 dBm. Ground `--adapter 0e8d:7612@8-1 --ack-target 0
--uplink 0 --channel 100 --bw 40 --net-id 7`; drone `--adapter
0bda:a81a@1-1 --channel 100 --bw 40 --net-id 7 --peer-mac <ack_target>
--fixed-rung 2 --ring /p2pbench --txpwr-offset-qdb -40`; feed `--synthetic
--fps 30 --kbps 3000 --idr-every 30`.

Run 1 (`c0-firstlight-1`) exposed two bring-up bugs, fixed before run 2:
every IDR block declared lost (rule (3) dated from the first symbol —
26 of 763 blocks, exactly one per second), and a `--fixed-rung` drone never
fed reports to its controller (report_age MAX, FLOOR, no Ucast).

Run 2 (`c0-firstlight-2`, window 8–28 s) — **PASS** on what this pairing can
show:

| criterion | result | adversarial counterpart |
|---|---|---|
| `uniq` rising | 434 symbols/s vs 436 video frames/s submitted (~99.5 %); 760 blocks decoded, 0 failed | single ear, near field; 25 of 760 blocks (3.3 %) needed FEC and 100 ms loss windows reached 33 ‰ at −37 dBm — not a clean link, and the MT7612U reports no EVM, so front-end saturation cannot be excluded |
| `diversity ≈ uniq × (ears−1)` | n/a (one ear) | untested |
| `fec_fail_permille 0` | 0 throughout | — |
| `ack8 == 8`, retries < 0.5 | ack8 8 throughout, `retries_mean_centi` 0; probes 250/250 ok, 250/250 reported (coverage 1.00) | ON arm only — the OFF shape (no responder) is not yet measured; this is A2 (8812EU → MT7612U), one unit each |
| `report_age_ms < 200` | 19–21 ms, TRACK throughout | reports are group-DA from an MT7612U at MCS0/20 — no retry, so a loss is silent |
| `p2p.da` Ucast | Ucast at +1.5 s, stays | — |
| `tx_timeouts 0` | 0; `tx_failed` 0 | 25 s run; the 20 ms stall is C7's question |
| ping through the tun | drone→ground 39/40, avg 13.8 ms, max 62 ms; ground→drone 19/20, avg 7.8 ms (`c0-ping-1`) | one loss each way, unexplained; ground→drone IP is group-DA (no retry); RTT max reflects IP queued behind video |

Also seen: the drone's video queue bound dropped 3 frames in 20 s
(`q_video_ms` max 55) — the synthetic IDR every second at 3 Mb/s is a burst
the 60 ms bound clips at R2. Design behaviour, not a link fault, but a real
encoder's IDR size decides whether it bites.

The review fixes listed in the PR were made after first light; the first-light
setup was re-run on the fixed tree (`c0-firstlight-3`, 2026-10-29): 436
symbols/s, 0 failed blocks, `ack8` 8, Ucast at +1.7 s.

Same units, channel, power and feed as first light unless stated; "near
field" below means ground RSSI −35..−38 dBm with no attenuator, so no cell
here says anything about range.

### C1 — 8812EU soliciting vs 8812EU responder (A1) — NOT RUN

Needs a second 8812EU as the responder; this bench has one. A1 is half-
answered by C2 (the 8812EU closes the ACK loop as the soliciting TX against
an MT7612U responder, below) — the 8812EU-responder half is open.

### C2 — 8812EU → MT7612U ACK target (A2) — PASS (one unit pair, near field)

`c2-run1`, 2026-10-29. Ground `--uplink none` (ACK target only, rx-only
`Init`), drone `--fixed-rung 2`; ground SIGTERM'd at +30 s for the OFF arm.

| arm | probes reported / sent | coverage | ok | retries histogram | mean |
|---|---|---|---|---|---|
| ON (2.0–24.3 s) | 223 / ~223 | 1.00 | 223 (1.00) | {0: 220, 1: 3} | 0.01 |
| OFF (26.3–54.6 s) | 283 / ~283 | 1.00 | 0 | {3: 283} — all pinned at the limit | 3.00 |

`ack8` 8 → 0 in eight probes (0.8 s) after the kill. Group decode while
ACKing: 435 symbols/s vs ~436 video frames/s submitted, equal to the
first-light ground (434/s) — the auto-responder and the monitor filter
coexist on this MT7612U. Counterpart: one 8812EU, one MT7612U, at −36 dBm
with ~50 dB of margin; coverage 1.00 is at R2's ~450 frames/s, far below the
~1.3 k reports/s CCX ceiling that R5/R6 cross (C7 measures it there). The
MT7612U-only ground is **kept** in v1 (C2's flip does not trigger).

### C3 — 40 MHz-parked ground, 20 and 40 MHz frames (A3, A5) — PASS for the MT7612U ear only

`c3-r3`, `c3-r5`, 2026-10-29, 60 s each, window 8–50 s, ground uplink 0.

| drone | frames on air | ground uniq/s | video frames/s submitted | ratio | failed blocks |
|---|---|---|---|---|---|
| `--fixed-rung 3` (tune 20) | MCS3 / 20 MHz | 404.6 | 405.3 | 99.8 % | 0 |
| `--fixed-rung 5` (tune 40, floor 0x35) | MCS3 / 40 MHz | 368.2 | 369.4 | 99.7 % | 0 |

A5 (MT7612U receives at 40 MHz, incl. 20-in-40 on the primary) holds on this
unit. Not shown: `adapters == 2` and the 8812EU ear's ratio (no second
8812EU), so A3 for an 8812EU ear is open. Near field: a width-dependent
sensitivity cost would not show at this margin.

### C4 — wide_tx_igi_floor edge and uplink cost (A4) — edge: 0 on this unit; cost: NOT RUN

`c4-edge-v00-r{1,2,3}`, 2026-10-29: `--fixed-rung 5 --wide-igi-floor 0`, 20 s
each. All three starts aired the first 40 MHz frame at once (663–707
submitted at +1 s, 0 `tx_failed`, 0 timeouts, ground uniq rising within
~0.5 s of the drone's first frame). By the cell's rule the edge is **0 for
unit 1-1**. Counterparts, all of which limit it: the starts were soft
re-inits (card-disable → power-on in a fresh process), not VBUS cold — the
8812EU sits on an xHCI root port that uhubctl cannot switch; one unit, and
`src/jaguar3/CLAUDE.md` already records one 8812EU that never stalls and one
that needs the floor, so this says nothing about the drone's own unit; the
ground ear saw no foreign 802.11 on ch 100 but the drone's receiver did, so
"quiet" is not established. Floor 0 had 2–3 failed blocks of ~466 per run
(one per run is the drone SIGTERM mid-block), against 0 in C3's single R5 run
at 0x35 — inside the noise at n = 3 vs n = 1, recorded, not a finding.

The cost half (attenuation where ack8 ≈ 6–7, floor on vs off) needs an
attenuator or distance: the link sits ~50 dB above the edge, and
`--txpwr-offset-qdb` cannot remove 50 dB. The default stays 0x35 (per-unit
edge; no design flip). Re-run the edge on the drone's own 8812EU.

### C5 — blind descent and HOLD — STOPPED on two findings (design questions open)

`c5-run1`, 2026-10-29: drone adaptive `--start-rung 5`, ground MT7612U ack
target + uplink. (a) used the new SIGUSR1 report mute, because restarting
`p2p_rx` on a one-adapter ground also kills the ACK for ~1 s.

| leg | expected (HANDOVER) | observed (drone clock) | verdict |
|---|---|---|---|
| (a) reports muted at 20.00, ACK alive | budget R3 ≤ 500 ms, PHY R3 +500 ms, then HOLD | HOLD at 20.62 with **no step**: R5/R5 kept; DA → Bcast at 20.62 | contradicts HANDOVER; matches the code and `link_ctl_test` ("Probes ACKed all along: Hold at the report timeout, no initial drop") |
| unmute at 35.00 | TRACK, no snap-back | TRACK at 35.22 at R5; Ucast at 36.22 | PASS |
| (b) ground SIGTERM at 54.97 | budget R−2 at +500 ms, R−2 per 500 ms, floor ≤ ~2.5 s, ack8 → 0 in 8 probes | Bcast 55.32; budget R3 55.52; then 56.02 / 56.52 / 57.02 steps; FLOOR at 57.02 (+2.05 s); ack8 4 → 0 by 56.02; tune 20 MHz at 59.09 (3.07 s at ≤ R3) | PASS — every step on the 500 ms grid |
| (c) ground restarted at 64.96 (up 65.90) | first fresh report → TRACK, no snap-back | TRACK at 66.02, R0 kept; Ucast at 67.52 | PASS (needs the session-nonce fix — before it, the restarted ground's epoch 1 was refused as stale) |

**Finding 1 — promotion can never fire.** In 20 s of clean TRACK at R5 the
drone never went to R6, and after (c) it sat at R0 for the remaining 29 s on
a −36 dBm link. `LinkController::probe_evidence_ok` requires frames at rung
r+1 in the ground's report window and vetoes on none, but nothing is ever
sent at r+1 — DESIGN.md "Sensors" sends the probe at the current rung. So any
outage parks the link at the floor for the rest of the flight. The unit tests
pass because they feed synthetic reports with r+1 counters.

**Finding 2 — HOLD entry.** DESIGN.md "BLIND … HOLD exception: ack8 ≥ 6 →
stop descending" is read two ways: the code holds at the report timeout
without a step, HANDOVER C5(a) expects one R−2 step first.

Both are design decisions; proposals are on PR #4, no code changed for them yet.

### C6 — DA machine vs a 10 pps IP class, ground unplugged — PASS

`c6-run1`, 2026-10-29: ground in netns with tun 10.99.0.2, drone adaptive
from R5 with tun 10.99.0.1, `ping -i 0.1` throughout; ground SIGTERM at 24.19,
restarted at 34.22 (drone clock).

- Bcast at 24.50 (+0.31 s; pass ≤ 1.5 s).
- `tx_duty_permille` in the 0.6 s after the kill, before any PHY step: 85–296,
  against 82–287 in the 5 s before (max is the IDR burst both times) — no
  storm. After that the duty rises with the blind descent to R0 (707 at the
  floor), which is the rung, not the DA.
- `dropped_ip_pkts` 0 throughout.
- Ping: 100 % before the kill, 0 while the ground was gone, 96 % from +3 s
  after the restore; first reply 0.88 s after the restart (group DA, before
  TRACK); TRACK at 35.30, Ucast at 36.90 (≥ 1 s of ack8 ≥ 7 after TRACK).

Counterparts: the run stayed at R0 after the restore because of C5 finding 1,
so Ucast was re-entered at the floor rung only; one run.

### C7 — 20 ms-stall soak (A7) — PASS (one unit pair, near field)

`c7-r5` 10:46–11:16 and `c7-r6` 11:16–11:46, 2026-10-29, ch 100 HT40+, drone
8812EU 1-1 at `--txpwr-offset-qdb -40`, ground MT7612U 8-1, no attenuator.
`run_cell.sh` with `--fixed-rung 5` / `6`, feed `--synthetic` at the budget
then in force (19 876 kb/s at 120 fps; 36 017 kb/s at 240 fps), bmc
`tx.report` lines filtered out of the log. Logs:
`p2plink-bench/logs/20261029/c7-r{5,6}.*`; analysis `c7_analyze.py`.

| | R5 | R6 |
|---|---|---|
| longest flat `tx_submitted` (pass < 2 s) | 0 ms | 0 ms |
| frames/s | 2 379 | 4 325 |
| `tx_failed` / `tx_timeouts` | 0 / 0 | 0 / 0 |
| `q_video_ms` max (bound 60) | 7 | 12 |
| `dropped_video_frames` | 15 | 11 |
| stats cadence p50 / max | 100 / 108 ms | 100 / 100 ms |
| clean exit (`p2p.exit`) | yes | yes |
| ground failed blocks | 53 of ~215 k | 167 of ~431 k |
| probe report coverage (ok rate) | 0.88 (1.000) | 0.76, worst minute 0.67 (0.998) |
| `tx_duty_permille` p50 / max (cap) | 707 / 1107 (500) | 822 / 1125 (450) |

Counterparts:
- The `q_video` maximum is the strongest stall evidence here: at 70–80 %
  model duty, a TX stall of ~10 ms or more would have queued that much
  airtime, so neither leg had one in 30 min. That is at the budget's load,
  not at saturation, and on one unit.
- Report coverage falls with frame rate (0.88 at 2.4 k/s, 0.76 at 4.3 k/s):
  the CCX report path does not keep up, so `ack8` sees fewer probes at R6. A
  missing report is UNKNOWN, not a failure; the review's deferred CCX
  saturation item.
- The duty overran its cap on both legs: the budget formula ignored the
  86 µs per-frame overhead. Fixed in c8d8408 (budget from the airtime
  model); this run used the old budget, so it soaked ~1.6x and ~2.5x the
  load the new budgets feed.
- An 8812BU witness (`witness.sh`, rxdemo `rx.busy`, passive) read 61 % busy
  airtime during R6 at 4 308 frames/s, against 62–64 % predicted from
  frames/s × (PLCP + data): the model's on-air part agrees; its 50 µs IFS
  share is idle time a busy meter cannot see.
- Feed ring-full drops (647 / 1 324) are all before the drone attached or
  after it stopped.
- The "without `no_cancel_multipkt`" arm was not run (the knob is hard-wired).

### C5 re-run — promotion (13db3fc + 435c414) and 20↔40 retunes — PASS (one unit pair, near field)

`c5p-run1`, 2026-10-29 12:05, drone adaptive from `--start-rung 0`,
`--txpwr-offset-qdb -40`, feed 3 Mb/s at 30 fps; ground SIGTERM at +60 s and
+150 s, restarted at +70 s and +160 s. `c5p.sh`, `c5p_analyze.py`.

- Climb R0 → R6 three times in one session, one rung per promote dwell
  (5.12 / 10.14 / 16.18 / 21.23 / 26.26 / 31.34 s on the first climb), the
  budget step 0.5 s after each PHY step. Finding 1 above is closed.
- Each R3 → R4 promote retuned the drone 20 → 40 MHz in the same tick
  (21.23, 95.68, 211.41 s) and 40 MHz frames continued without a gap: per-100 ms
  `tx_submitted` 25–49 around every retune (the ~100 rows are the 1 Hz IDR), no
  zero-frame 100 ms row in the whole 240 s, 0 failed, 0 timeouts. This is the
  "IGI clamp when the floor drops / first wide frame after a retune" path that
  `docs/experiments/8822c-ht40-tx-stall.md` lists as never exercised on air.
- Each ground kill: Bcast +0.3 s, budget R4 +0.5 s, then the 500 ms grid to
  FLOOR in 1.7 s, tune 20 MHz 2.6 s after FLOOR.

Counterparts: one session, one unit, near field, a fixed 3 Mb/s feed (above
the R0 budget, so duty sat at 480–760‰ at the floor). Ground restart 2 came
up in 0.9 s; restart 3 took **31 s** — 11 MT7612U `mcu command timed out`
during bring-up under the drone's load (the known gap in §5); measured
separately below.

### Budget on hardware (c8d8408) — R5 PASS, R6 5 % over in the model

`bud-r5` / `bud-r6`, 2 min each, fixed rung, feed exactly the new budget
(14 095 kb/s at 120 fps; 19 600 kb/s at 240 fps), 8812BU witness running.

| | R5 (cap 500‰) | R6 (cap 450‰) |
|---|---|---|
| model `tx_duty_permille` p50 | 495 | 472 |
| witness busy (signal on air) | 43 % (frames/s × on-air: 42.8) | 38 % (35.7) |
| `q_video` max / video drops | 0 ms / 0 | 0 ms / 0 |
| ground failed blocks | 8 | 0 |

C7 fed the old budgets and read 707 / 822‰. R6 stays 5 % over in the model
because block padding at 240 fps (a 10.2 kB frame is 7.4 symbols → 8 + 2) is
more than the 0.97 margin covers; at 120 fps it is. The saturation runs below
put the model ~12 % over the real per-frame cost at R6, so the real share is
~420‰. A frame-rate-aware margin is a possible follow-up, not done.

### 8812EU HT40 TX at saturation (libc0607/rtl88x2eu-20230815#7)

The issue: a kernel-driver 8812EU injecting HT40 under load periodically stops
draining (the wait sits between URB submit and completion), RX sees bursts of
loss, and peak rate varies between bring-ups. Bench: fixed rung, feed far
above airtime (R3/R4 30 Mb/s, R5 60, R6 100), 60–120 s per bring-up,
`sat40.sh` / `pwr40.sh` / `rep40.sh`, `cal_analyze.py`.

| arm | power | bring-ups | sagging | frames/s (steady) |
|---|---|---|---|---|
| R3 (20 MHz MCS3, 26 Mb/s) | −40 qdB | 3 | 0 | 1 890 |
| R4 (40 MHz MCS1, 27 Mb/s — the issue's setting) | −40 qdB | 3 | 0 | 1 950 |
| R5 (40 MHz MCS3) | −40 qdB | 3 | 0 | 3 500 |
| R6 (40 MHz MCS5) | −40 qdB | 3 | 0 | 5 750 |
| R4 | −20 qdB | 2 | 0 | 1 950 |
| R6 | −20 qdB | 8 | **3** | 5 750 |

"Sagging" = 100 ms rows below half the median; clean runs never fell below
0.97 × median. In the three sagging runs (`pwr-r6-2`, `rep-r6-3`, `rep-r6-4`)
the rate fell to 64–500 per 100 ms in episodes of seconds, recurring through
the session (`rep-r6-4` lost ~11 s of 115 s of airtime); the ground's `uniq`
fell in step with loss ≤ 29‰, so the frames were not sent rather than lost;
0 `tx_failed`, 0 `tx_timeouts`. Not retries: video is group-DA (never
ACKed), and the 13 unicast probe reports inside the `pwr-r6-2` sag show 0
retries. Not the witness: `rep40.sh` ran without it.

Counterparts: near field (50 cm, ground −29..−33 dBm at −20 qdB), so higher
power and proximity are not separable here; one unit; at saturation only —
C7 (budget load, −40 qdB, 1 h) and the budget runs saw no sag. The 3/8 vs 0/3
power split is too few runs to call.

**Isolation — not devourer's HT40 TX path.** The same air shape from
devourer's own `txdemo` (MCS5/40 LDPC, ~1400 B, zero gap, −20 qdB, ch 100,
no ground on the air), fresh bring-up per run, 500 k frames each
(`txmat.sh`, `txmat2.sh`):

| arm | bring-ups | sagging |
|---|---|---|
| txdemo, 40 MHz tune | 6 | 0 |
| txdemo, CCA gates off (`DEVOURER_CCA_GATES=1,1`) | 6 | 0 |
| txdemo, 40-in-80 tune | 6 | 0 |
| txdemo + `DEVOURER_TX_REPORT=1` (a CCX report per frame) | 5 | 0 |
| txdemo + TX report + `DEVOURER_TX_WITH_RX=thread` | 5 | 0 |
| p2p_tx R6 −20 qdB, **no ground on the air** | 4 | 0 |
| p2p_tx R6 −20 qdB, ground up (above) | 8 | 3 |

The libc0607 stall is TX-only; 28 ground-less bring-ups never showed it, so
devourer's 8812EU does not reproduce it in this shape. The sag needs the
MT7612U ground 50 cm away (0/4 without it is weak on its own: at 3/8 that is
15 % likely by chance; with the 0/28 it is consistent). Which interaction —
the drone's DIG/EDCCA reacting to the ground's −30 dBm frames, or something
on the ground side — is not separable without an attenuator. **Re-check at
real separation** (the ssc338q airframe) before acting on it; the next
discriminator here would be a p2p_tx CCA-off arm with the ground up. txdemo's
`tx.stats` carries no time field — `stamp.py` stamps lines for round 2; round
1 is judged by run duration (107–108 s for every run; a 4 s sag adds 4 s).

Two side results:
- **The HT40 rate correction holds on air**: R4 / R3 frames/s = 1.032 at
  saturation, PHY 27 / 26 = 1.038 (with 26/26, as the ladder had, they
  would be equal).
- **The model's 50 µs IFS is an upper bound on this unit**: mean idle per
  frame at saturation 49 µs (R3), 53 (R4), 38 (R5), 30 (R6), so the model
  overcharges R5/R6 frames by 4–12 % (reads 1 040–1 120‰ at 100 % airtime)
  and the caps there are slightly conservative. The constant stays.

Witness note: the 8812BU witness (`build/rxdemo`) logged `rtw_read …
iostream error` in every R4 run and once at R6, and in `sat-r4-1` an uncaught
`std::ios_base::failure` aborted it. Not a drone or ground number; a devourer
Jaguar2 robustness issue, queued separately.

### Repair floor (86594f5) on air — PASS

`floor-r5`, 60 s, R5 fixed, feed 1 Mb/s at 30 fps (P-frames ~2.6–4.9 kB, so
k = 2..4, where the ratio alone gives 1 repair). Drone: 9 969 frames for
1 664 blocks + 550 probes = **5.66 symbols per video block**; the feed's own
size distribution predicts 5.85 with the floor and 4.89 without it. Ground:
1 failed block in 60 s, 0 `tx_failed`. Counterparts: one 60 s run, near
field; this proves the encoder airs k + 2, not that the floor recovers a
loss the ratio would not — that needs a lossy link.

### MT7612U ground restart under drone load — found and fixed (devourer core; upstream PR)

The C5 re-run's 31 s ground restart, chased with `gr_restart.sh` (ground up,
drone fixed R5 at budget, then the ground SIGTERMed and restarted N times while
the drone airs; `gr_analyze.py`). Counts re-done from the runner logs (the
first write-up and 3d7c081's message miscounted them as 16/32 and 7/10):

| arm | slow restarts (31–34 s, 10–11 MCU timeouts) |
|---|---|
| no drone | 0 / 4 |
| first ground start, before the drone (every arm) | 0 / 9 |
| restarts under the drone, −40 qdB (4 sessions pooled) | 16 / 22 |
| restarts under the drone, −120 qdB (2 sessions) | 6 / 10 |
| first fix attempt (2nd flush in `mt_mac_stop`) | 8 / 10 — did not help |
| EP 4 drain, all revisions | **0 / 31** (0.94–1.26 s; 10 of them on the final code) |

Bimodal, not graded. The −120 qdB arm lowered the ground's level by only 6 dB
(−37 → −43 dBm; the 8812EU offset floors), so level was not the variable.
Mechanism: during `MCU_CAL_RXDCOC` received data reaches EP 4 with
`MT_MAC_SYS_CTRL` = 0 (MAC RX off) and the USB RX bulk enable set — a register
probe and one decoded EP 4 read (a data frame from the drone's MAC) show it —
and with EP 4 undrained the chip does not deliver the EP 5 replies. The fix
drains EP 4 once per command on an MCU reply timeout while no RX ring or sync
drainer owns it (`mt7612u_dev::ep4_owner`). Reviewed by three independent
agents over two rounds (a use-after-free in the first revision's unlocked
`d->a->rx_active` read was caught there and is gone). Upstream reproducer
`tests/mt7612u_busy_bringup.sh` (rxdemo + txdemo, upstream code only): master
09c2ff3 stalled 6/6 at 20 MHz and 6/6 at HT40 under a zero-gap MCS3 peer; with
the fix 10/10 and 10/10 at 1.18 s. Detail: `docs/mt7612u.md`, "Bring-up on a
busy channel". The drone-side "no ACK for 30 s → duty 0.3" rule (§5) is not
needed for this and stays unimplemented. Counterparts: one MT7612U unit, one
peer model, one channel, near field.
