# p2plink design (v1) — range-first 1:1 video + IP link on devourer

Objective: range and link robustness, not throughput. Every control action must
be unable to cause data loss on its own (no flapping, no blind width/channel
moves, no retry storms, no encoder overshoot).

## Roles
- Drone: one RTL8812EU. Transmits video (group DA, FEC), IP (DA per DaMachine),
  probes (unicast to the ACK target). Receives link reports + IP.
- Ground: 1..N adapters. Slots: `ack_target` (whose MAC the probe addresses;
  auto-ACKs — MT7612U factory MAC or 8812EU via SetAckResponder), `uplink`
  (transmits reports + IP; 8812EU preferred; an MT7612U may, broadcast only —
  its unicast TX is a 75 fps cliff), `ears` (monitor only). One MT7612U may fill
  all three. Assert probe DA == ack_target permanent MAC and DA ∉ other ears.

## Fixed radio parameters (construction-time, both ends)
retry_limit 3, ack_timeout_us 128, A-MPDU off, rx.pool_exhaust backpressure
(Realtek), tx.no_cancel_multipkt=true (drone), tx.report=1 (drone), LDPC on,
LGI, 5 GHz only, static channel per boot (operator-manual switch only).
Ground parks at 40 MHz; the drone picks 20/40 PER FRAME in radiotap (the
receiver decodes a 20 MHz frame on its primary with no retune; measured 0 dB
wide-RX penalty). Frame MPDU 1400 B (one IP packet per frame; MT7612U RX cap
3836). No 5/10 MHz anywhere.

## Ladder (rung = {frame bw, MCS, FEC k/n, duty cap}); LDPC always
```
rung bw  MCS PHY(Mb/s) k/n   duty  notes
 R6  40  5   104       8/10  0.45
 R5  40  3   52        8/10  0.50
 R4  40  1   26        8/11  0.55
 R3  20  3   26        8/11  0.60  drone retunes its own tune to 20 MHz after >=3 s at <=R3
 R2  20  2   19.5      8/12  0.60  (drops the wide-TX IGI floor = recovers uplink RX sens.)
 R1  20  1   13        8/12  0.60
 R0  20  0   6.5       8/12  0.60  FLOOR; encoder resolution/fps co-designed for bpp here
```
Video budget(r) = PHY × duty × k/n × 0.97 − control reserve; calibrate per rung.

## Sensors (drone)
- `ack8`: ok count over the last 8 REPORTED probe tx.reports (state 0 = ok,
  state 1 = retry-pinned). A missing report is UNKNOWN and not counted.
- `rep_age`: ms since the last LinkReport with a new epoch.
- `q_air_ms`: video host queue depth expressed in airtime at the current rung.
- `tx_timeout`: TxStats.last_was_timeout (chip FIFO full).
Probe: Type::Probe, <=32 B, unicast to ack_target, every 100 ms in EVERY state,
at the current rung's rate. It is the primary uplink sensor (arrives SIFS after
our own frame, when our MAC is listening; the 10 Hz report is secondary).

## LinkController states
TRACK (rep_age < 500 ms)
- acute: report loss >= 200‰ (>= 32 uniq) → budget+FEC := floor NOW, MCS/frame
  := floor after 0.5 s.
- persistent: leaky score (+1 per 100 ms window with loss >= 45‰, −1 clean);
  at 5 → r−1 (bitrate leads 0.5 s), lock vacated rung 30 s.
- local: q_air_ms > 60 or tx_timeout → r−1 immediately.
- promote: >= 10 consecutive fresh reports AND loss < 20‰ AND rssi_best >=
  rung_floor_dbm(r+1) + 6 AND probe-rung PER <= 2× current PER AND dwell >= 5 s
  AND rung not locked → r+1; bitrate lags 0.5 s; never skip rungs.
- promotion evidence: the probe-rung PER comes from video REPAIR symbols the
  drone airs at r+1 from 1 s before the dwell ends (TRACK, settled, r+1
  unlocked and strictly faster). A lost repair costs nothing while the block's
  sources arrive. Every symbol's flags carry the repair rung, so the ground
  charges each lost symbol to the rung it aired at. A step that is not faster
  on air (R3 → R4: 26 = 26 Mb/s, 20 → 40 MHz) needs no r+1 evidence.
  (C5 on hardware: with the probe at the current rung and nothing else at
  r+1, promotion could never fire.)
BLIND (rep_age >= 500 ms)
- t0: budget+FEC := max(r−2, floor); t0+500 ms: MCS/frame := r−2; then every
  500 ms r := max(r−2, floor). Floor from R6 in ~2.5 s.
- HOLD exception: ack8 >= 6 → stop descending (ground hears us; its reports
  don't reach us). Evaluated before the t0 step, so with the probes ACKed at
  the report timeout BLIND goes straight to HOLD at r — no initial r−2 (C5(a)
  confirmed on hardware; descending does nothing for a lost reverse path).
  Never promote in HOLD. ack8 <= 2 or no reported probe for 1 s → keep
  descending.
- exit: first fresh report → TRACK (no snap-back).
FLOOR: as BLIND at r = floor. Stays. (No rescue rung in v1.)
Promotion asymmetry: demote within 0.5 s, promote only after 5 s dwell + evidence.

## DaMachine (IP and control classes only; video ALWAYS group DA)
BCAST → UCAST: ack8 >= 7 for >= 1 s AND LinkController in TRACK.
UCAST → BCAST: 2 consecutive retry-pinned probe reports, or ack8 <= 5, or state
!= TRACK. Min dwell 1 s each way. IP-bulk never unicast unless ack8 == 8.

## Budget / scheduler (single TX thread, strict priority)
1 control (probe, report, ctl)  2 IP-interactive (token bucket <= 15 % of rung
airtime at the CURRENT MCS)  3 video (queue bound 60 ms of airtime; drop
newest-first by SVC layer: Enhance first, never IDR/base, never control)
4 IP-bulk (leftover). Rate + FEC (k,n) + DA bind at DEQUEUE, never enqueue.
Encoder: bitrate target = budget(r); a demote re-targets the encoder 0.5 s
BEFORE the MCS step; during that lead thin parity to the floor k/n before
dropping source frames.

## RX merge (ground, N ears)
Key (stream, block_id, fec_idx) → first copy wins; later copies count as
`diversity`. Cursor window: blocks older than cursor−4 are dropped. A block
decodes when >= k symbols are present (FEC) or all k sources arrive (fast
path). Gap/finalize rules in order: (1) every non-silent ear has advanced past
the block, (2) a newer block_id has been seen, (3) 20 ms with no symbol of the
block (a quiet dwell dated from its LAST symbol — a block's own airtime exceeds
20 ms for every IDR and for most blocks at R0; first light lost every IDR block
when the dwell was dated from the first symbol). An ear with 0
frames for 200 ms while a sibling delivers is excluded from (1) and from
`adapters`. Hardware retry copies are dedup'd by the same key (the Retry bit
and seq12 are informational).

## Reporting layer
StatsSnapshot (report.h) published every 100 ms to a POSIX shm object
(/p2plink.<role>) and as one NDJSON line on stdout (`{"ev":"p2p.stats",...}`).

## Out of scope for v1
Encryption, channel migration protocol, cache/repair, multi-receiver authority,
calibration, narrowband, 2.4 GHz, A-MPDU.

## Implementation notes (v1 as built)
- Video block sizing: symbol payload S = frame_bytes − 18; k = clamp(ceil((12 + frame_len)/S), 1, 128),
  n = k + max(1, ceil(k·(n_r−k_r)/k_r)) ≤ 192. A frame above 128·S (≈176 KB) is dropped whole and counted
  (`dropped_video_frames`) with one warning; never truncated or split.
- Tune vs rung: while the drone is tuned to 20 MHz and the controller's PHY rung is a 40 MHz one (the
  promote edge), frames air at R3 (same 26 Mb/s, 20 MHz) until the 100 ms tick retunes to 40 MHz.
- Tun MTU 1380 on both ends (1400 − 18 header − 2 packed-length prefix); ground IP is group-DA only.
- Rx-only ears have no bring-up completion signal (`Init` is bring-up + blocking worker); the glue waits a
  bounded 6 s for an early exception, then treats the ear as up and reports later worker death via `rx_failed()`.
- One local (queue/timeout) demote per 500 ms, so the airtime inflation a demote causes cannot cascade.
- Peer restart: header byte 11 (formerly reserved 0) carries a per-sender session nonce, random 1..255
  per adapter open. Seq dedup windows restart when it changes, and the drone treats a LinkReport from a
  new ground session as the first report (`LinkController::on_peer_restart`) instead of refusing it as
  stale until the new epoch overtakes the old one. A forward epoch whose cumulative uniq went backwards
  carries no acute-loss evidence.
