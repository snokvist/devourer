# Phase 0 — does the MT7612U unicast cliff block station mode?

**No. Gate PASS.** A station-shaped transmitter runs at 2084 fps with a 99.9%
ACK rate — 71% of its matched broadcast control. The documented 40× unicast
cliff is real, and it is a property of **TX-only injection**, not of the part
and not of a station.

An earlier revision of this document concluded the opposite, on a measurement
that was structurally incapable of testing its own claim. Two adversarial
reviews caught it. The wrong conclusion and how it was reached are kept at the
bottom, because the failure mode is more instructive than the result.

Harness: `bringup ucast <chan> <secs> [peer-mac] [bytes]`
(`src/mt7612u/tools/bringup.cpp`, `gate_ucast`).

## Rig

| role | part | where |
|---|---|---|
| DUT (transmitter) | MT7612U, chip `0x7612`, MAC `40:a5:ef:5a:32:f8` | USB `7-1` — a **USB 2.0** link (`dmesg`: "reset high-speed USB device") |
| Peer (ACK responder) | RTL8812AU, EFUSE MAC `20:0d:b0:c4:a7:6a` | USB `1-13` |

Peer: `rxdemo`, `DEVOURER_ACK_RESPONDER=02:4d:54:76:12:0a`,
`DEVOURER_CHANNEL=149`. Channel 149, HT MCS7, 20 MHz, 1400-byte QoS data,
`wcid = 0xff`, 5 s per arm, ~20 cm separation, `io errors after bring-up: 0`.

## Result

```
  arm  configuration                 fps     Mbit/s    busy%  done/err
  A    broadcast,       No Ack      3043      34.08    79.1   15199/0
  B    ucast nobody,    Normal        22       0.24    16.4      44/50
  C    ucast nobody,    No Ack        41       0.46    11.5     166/24
  D    ucast PEER,      Normal        22       0.25    16.4      46/52
  E    ucast PEER,      No Ack        56       0.63     7.1     266/0
  F    ucast PEER, ownSA Normal       22       0.25    16.4      46/50
  G    broadcast,  ownSA No Ack     2934      32.86    50.2   14653/0
  R    ucast PEER, ownSA, RETRIES=0    57            (0x47f01f0f -> 0x47f00000)
  S    ucast PEER, ownSA, SYNC          5            28 ok / 8 failed
  T    ucast PEER, ownSA, MAC RX ON   2084   10421 sent, 10406 ACKs, 21510 seen
```

**Arm T is the result.** It is the only arm that is a station: addr2 is the
adapter's own port identity, the MAC receiver is **on**, the peer is armed, and
the ack policy is Normal. 10406 ACKs addressed to our own TA against 10421
frames sent — **99.86%** — at 2084 fps, against its matched own-SA broadcast
control G at 2934 fps. **71% of control, with essentially every frame
acknowledged.**

**Arms A–G ran with the MAC receiver disabled**, which is what makes them the
cliff rather than a measurement of one. `gate_ucast` starts the MAC with
`MT_RX_DRAIN_NONE`, and `mt_mac_start` sets `MT_MAC_SYS_CTRL_ENABLE_RX` only
when the caller will drain EP 4 (`src/mt7612u/init.cpp:290-296`) — a
deliberate deviation from mt76, documented in that comment, which exists so a
TX-only injector cannot wedge the part below the USB level. The consequence is
that **no ACK can be consumed in arms A–G, so every unicast frame runs its
retry ladder to exhaustion by construction.**

The arithmetic fits, from the values this driver programs:

- `MT_TX_RETRY_CFG = 0x47f01f0f` (`src/mt7612u/initvals.h:32`) → short retry
  limit 15, long-retry threshold 2032 bytes. Our 1404-byte MPDU is under the
  threshold, so **15 retries** applies.
- `MT_WMM_CWMIN = 0x2344`, `MT_WMM_CWMAX = 0x34aa`
  (`src/mt7612u/init.cpp:183-184`) → AC_BE **CWmin 15, CWmax 1023**.
- 9 µs slot at 5 GHz. Mean backoff summed over a ladder doubling
  15, 31, 63, 127, 255, 511, then 1023 for the remaining nine rungs:
  **≈ 45.9 ms**. Plus ~16 × (209 µs frame + SIFS + ACK timeout + AIFS) ≈ 5 ms.

**≈ 51 ms predicted; 45.5 ms measured (22 fps).** The exponential-backoff
ladder explains the cliff to within a few percent. There is no ~45 ms constant
anywhere in the subtree to blame instead (`CTRL_TIMEOUT_MS` 300, sync bulk TX
500 ms, async TX 1000 ms).

`busy%` corroborates once read correctly: backoff is CCA-*idle*, so a ladder is
mostly idle. Arm D's 16.4% is about 16 transmissions × 209 µs × 22 fps ≈ 7.4%
plus EIFS after each unanswered attempt plus ambient. A single un-retried frame
at 22 fps would be 0.46% busy — the channel is ~35× busier per delivered frame
than one transmission, which is the ladder, measured.

### What the other arms add

- **R (retry limits zeroed): 57 fps, not the ceiling.** Zeroing both limit
  fields in `MT_TX_RETRY_CFG` lifted unicast 22 → 57 fps, a 2.6× improvement
  that stops well short of the 2934 fps control. So the retry *count* is not
  the whole story with the receiver off — the per-attempt ACK wait and CW
  escalation remain. This arm is a partial confirmation, not a clean one, and
  is reported as such.
- **S (synchronous path, no async ring): 5 fps with bulk-OUT timeouts.** With
  `d->a` cleared, `mt_tx_raw` takes the 500 ms synchronous bulk write
  (`src/mt7612u/tx.cpp:241`) and 8 of 36 writes returned
  `LIBUSB_ERROR_TIMEOUT`. The synchronous path is much worse than the ring for
  unicast-without-a-receiver, which is worth knowing but is not the mechanism.
- **F vs D: no difference, and the arm could not have shown one.** Transmitting
  from the port identity changed nothing *while the receiver was off*, because
  the MAC could not terminate the ladder either way. F only becomes meaningful
  in arm T's configuration, where it is one of the things that changed.
- **C and E (No Ack): 41 and 56 fps.** `txwi.ack_ctl` REQ is cleared
  (`src/mt7612u/tx.cpp:168`) and the QoS policy says No Ack, yet these sit at
  ~11–13 rungs of the same ladder rather than at the ceiling. Either the txwi
  bit does not reach the retry engine or the MAC derives ack policy from a
  unicast RA regardless. **Unexplained, and a concrete devourer-side defect
  candidate** — it is the reason `docs/mt7612u.md` records that No Ack "does
  not help".

### Rig validation

- Arm A reproduced the published figure exactly: **3043 fps / 34.08 Mbit/s**
  against `docs/mt7612u.md`'s 3037 fps / 34.01 Mbit/s.
- Peer health was checked independently, because a burned PA would produce the
  same numbers for an unrelated reason. Peer RX: 185 frames at `len 1404` (our
  MPDU + FCS) at RSSI −18 dBm. Peer TX: the MT7612U received **12879 OFDM
  frames** from the 8812AU running `txdemo` over 30 s. Both directions healthy.
- `done/err` per arm is now printed. Arms B/D/F show roughly as many URB errors
  as completions (44/50, 46/52, 46/50) while A, G and T show zero — the
  unicast-without-receiver arms are partly *failing* transfers, not merely slow
  ones, which the first revision could not see.

### Adversarial counterpart

Arm A is noisy: 2018, 2161, 3042, 3043 fps across runs of the identical
configuration. Ambient occupancy on ch149 and the USB 2.0 link were not
controlled. Arm T is therefore graded against arm G (own-SA broadcast, same
session) rather than against arm A. The unicast-without-receiver arms are the
opposite of noisy — 22 / 41 / 56 fps, stable to ±1 across every run.

Arm T's peer is devourer's own hardware ACK responder on an RTL8812AU, **not a
real AP**. The plan requires a repeat against hostapd on non-MediaTek silicon
before this result is relied on beyond Phase 0; that is now folded into
Phase 5's independent-witness requirement rather than blocking here, because
the mechanism is understood and the arithmetic is closed.

## Consequences

1. **Station mode on MT7612U is not blocked.** The project continues. Phase 1
   is unblocked.
2. **`docs/mt7612u.md`'s unicast-cliff section needs a follow-up**, not a
   correction. Its measurement stands and its guidance ("a one-way injected
   link must address frames to broadcast or multicast") stands — that is still
   right for an FPV downlink with no receiver and no peer. What it lacks is the
   scope: the cliff is a property of TX-only injection with
   `ENABLE_RX` clear, and it does not apply to a transmitter whose receiver is
   on and whose peer answers. Its stated cause — "the MAC arms an ACK timeout
   for a peer that never answers" — turns out to be exactly right, and the
   missing half is that giving it an answer fixes it.
3. **`gate_ucast`'s ordering requirement is a preview of the station seam.**
   Arm T has to start the async ring, then `mt_mac_start(MT_RX_DRAIN_RING)`,
   then the monitor filter. That is the same ordering constraint Phase 2's
   `SetStationIdentity` inherits, and `docs/station-mode-scope.md` already
   carries it.
4. **Open, and now the most interesting question:** why do arms C/E (No Ack)
   not reach the ceiling? A station does not care, but an injector does, and it
   is the difference between the current broadcast-only guidance and a usable
   one-way unicast link.
5. `MT_TX_STAT_FIFO` (mt76x02 0x1718) is not declared in
   `src/mt7612u/regs.h` and `txwi[19]` is always 0 (`src/mt7612u/tx.cpp:194`),
   so the driver cannot observe its own retry count. Both sides of this
   argument had to *infer* the ladder. Declaring that register would make it
   observable and is cheap.

## Follow-ups this run generated

- `mt7612u_link_stats()` reads die temperature over the MCU, and under
  sustained TX that read times out on every arm boundary (`mcu command timed
  out`, `drained 2 stale MCU replies before cmd 31`). Harmless here; worth
  fixing or documenting at the call.
- `bringup rx` reports `GATE F: FAIL - no frames received` in conditions where
  `bringup arx` receives 12879. It warns that it blocks the only EP 4 drainer
  but still prints a hardware verdict; it should refuse to grade.
- Kernel monitor injection via `AF_PACKET` measured 690k fps, i.e. the socket
  queue, not the radio — mac80211 injection does not backpressure. Any kernel
  A/B has to be witness-counted. Phase 0b as originally specified is the wrong
  experiment and is dropped; arm T answered the question directly and more
  cheaply.

---

## Appendix: the wrong conclusion, and why it survived one review

The first revision of this document concluded **FAIL** — "the cliff survives
everything", and "most probably a defect in devourer's MT7612U TX path". It was
wrong, and worth recording how.

The measurement had five arms (A–E) plus an ACK-counting arm V. All of them ran
with the MAC receiver off, which nobody noticed, so the claim "an actually-ACKing
peer does not change this" had **no arm that could have shown otherwise**. Arm V
appeared to rescue it by counting 51 real ACKs — but arm V transmitted with the
invented addr2, so the MAC would have rejected those ACKs even had it been
listening. It proved the peer emitted ACKs and nothing about the DUT.

The retry ladder was dismissed in one sentence — "not plausibly a retry ladder
at this frame rate" — with no arithmetic. The arithmetic takes ten minutes and
lands within a few percent of the measured number. The `busy%` argument was
offered as corroboration with its sign inverted: backoff is idle time, so low
busy% is what a ladder looks like, not evidence against one.

The first review (Flash) found the submission-vs-completion ambiguity and the
missing `mt_async_stats`, which produced the `done/err` column and arm S. It did
not find the disabled receiver. The second review found it, did the arithmetic,
and identified arm T as the missing configuration — the one the gate should have
had from the start.

The instructive part is the shape of the error: the document needed the cliff to
be host-side for station mode to survive, and it got there by eliminating the
MAC-side explanation without doing the work to eliminate it. The result it was
reaching for turned out to be true. The reasoning that got there was not, and
one reviewer was not enough to catch it.
