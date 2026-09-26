# MT7612U transmit retries, measured off the chip

`docs/station-mode-phase0.md` closed its argument with arithmetic — 15 retries
from `MT_TX_RETRY_CFG`, CWmin 15 / CWmax 1023 from `MT_WMM_CWMIN/CWMAX`, a 9 µs
slot, summing to ~46 ms against 45.5 ms measured. Good fit, still an inference,
and two reviewers argued about it because nothing in this tree could read the
number the MAC already knows.

It can now. `MT_TX_STAT_FIFO` (0x1718) and `MT_TX_STAT_FIFO_EXT` (0x1798) are
declared in `src/mt7612u/regs.h`, and `MT_TXOPT_TXS` sets the txwi pktid so the
MAC files a per-MPDU status entry. Harness: `bringup txs <chan> <frames> [peer]`.

The library's normal send path leaves pktid 0 and files no status, which is
why the opt-in exists: an undrained status FIFO is traffic nobody wants on a
path that must stay free of per-frame register I/O.

## Result

MT7612U at USB `7-1`, ch149, HT MCS7 20 MHz, 1400-byte QoS data, `wcid = 0xff`,
40 frames per arm. Peer: RTL8812AU running `rxdemo` with
`DEVOURER_ACK_RESPONDER=02:4d:54:76:12:0a`.

```
  MAC receiver OFF
  arm  configuration                fps entr/sent  success mean rtry    max
  a    broadcast,       No Ack        7   38/40         28       3.8     16  UNSETTLED
  b    ucast peer,      Normal        8   28/40          0      16.0     16  UNSETTLED
  c    ucast peer,      No Ack       51   40/40         40       0.0      0
  d    ucast peer ownSA Normal        8   28/40          0      16.0     16  UNSETTLED

  MAC receiver ON
  a    broadcast,       No Ack      889   40/40         40       0.0      0
  b    ucast peer,      Normal        7   15/40          0      16.0     16  UNSETTLED
  c    ucast peer,      No Ack        7   24/40         15       6.0     16  UNSETTLED
  d    ucast peer ownSA Normal      655   40/40         40       0.0      0
```

`fps` here is submit-and-settle over a deliberately small frame count, so it is
**not** the steady-state figure `gate_ucast` reports; the retry columns are the
point. `UNSETTLED` means the MAC still owed status when the per-arm deadline
expired — those rows are partial and are read only for the retry *value*, never
for the counts.

## What it settles

**1. The ladder runs to exhaustion, and it is 16.** Every unacknowledged
unicast arm reports **mean retry 16.0, max 16, zero successes**. The Phase 0
arithmetic is confirmed by the chip rather than by algebra. `MT_TX_RETRY_CFG`'s
short-retry limit is 15, so 16 is the limit plus the initial attempt.

**2. Arm d is the cleanest A/B in this whole investigation.** Identical frame,
identical addressing, identical rate — the *only* difference is
`MT_MAC_SYS_CTRL_ENABLE_RX`:

| receiver | entries | success | mean retry |
|---|---|---|---|
| OFF | 28/40 | **0** | **16.0** |
| ON | 40/40 | **40** | **0.0** |

With the receiver on, the station configuration succeeds on the **first
attempt, every frame, zero retries.** That is the Phase 0 arm-T result
confirmed from the transmitter's own accounting instead of from throughput.

**3. The No-Ack arms are NOT retrying — which kills the standing hypothesis.**
`docs/station-mode-phase0.md` listed arms C/E (txwi `ACK_CTL_REQ` clear *and*
QoS Ack Policy = No Ack, yet ~41–56 fps instead of the ~3000 fps ceiling) as
"~11–13 rungs of the same ladder" and flagged it as a devourer-side defect
candidate. **That was wrong.** Receiver-OFF arm c settles fully — 40/40
entries, **40 successes, 0.0 mean retries, max 0** — at 51 fps, which matches
the 41–56 fps those arms measured.

So the no-ack request *does* reach the retry engine: the MAC marks these frames
successful immediately and never retransmits. The ~20 ms per frame is real and
is **somewhere else entirely** — not the retry ladder, not the ACK timeout.

## The narrowed open question

Why does a unicast frame that the MAC completes successfully on the first
attempt, with zero retries, still cost ~20 ms, when the same frame addressed to
broadcast costs ~0.3 ms?

Candidates not yet separated: a per-WCID queue serialization with the
no-station index `0xff`, TXOP/EDCA admission for a unicast RA, or a USB
completion path that only retires these transfers lazily. `MT_TX_STAT_FIFO`
answers "how many attempts"; it does not answer "how long did each take".

This matters for injection, not for station mode — a station has its receiver
on and takes the arm-d path at 0 retries. It is the difference between
`docs/mt7612u.md`'s current "address one-way links to broadcast or multicast"
guidance and a usable one-way *unicast* link, which is why it is worth an
issue of its own.

## Caveats

- One adapter, one session, near-field, 40 frames per arm.
- Several rows are `UNSETTLED`. The settled rows carry the findings above; the
  unsettled ones agree on the retry value but their counts are partial.
- Receiver-ON arm c disagrees with receiver-OFF arm c (6.0 vs 0.0 mean retry)
  and is one of the unsettled rows. Unexplained; the receiver-OFF row is the
  one with complete accounting, and finding 3 rests on it alone. A repeat with
  a larger frame count should settle both before finding 3 is treated as
  closed.
- `txs_drain` does two register reads per frame inside the send loop, so this
  gate's pacing is its own and its `fps` column is not comparable with
  `gate_ucast`'s.
