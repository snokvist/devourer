# Phase 0 — does the MT7612U unicast cliff survive a peer that ACKs?

**Answer: it survives everything.** The gate returns FAIL, and the reason is
not the one the hypothesis predicted — it is not the ACK.

Harness: `bringup ucast <chan> <secs> [peer-mac] [bytes]`
(`src/mt7612u/tools/bringup.cpp`, `gate_ucast`).

## Rig

| role | part | where |
|---|---|---|
| DUT (transmitter) | MT7612U, EEPROM chip `0x7612`, MAC `40:a5:ef:5a:32:f8` | USB `7-1` — a **USB 2.0** root hub |
| Peer (ACK responder) | RTL8812AU, EFUSE MAC `20:0d:b0:c4:a7:6a` | USB `1-13` |

Peer armed with `rxdemo`, `DEVOURER_ACK_RESPONDER=02:4d:54:76:12:0a`,
`DEVOURER_CHANNEL=149`. Channel 149, HT MCS7, 20 MHz, 1400-byte QoS data,
`wcid = 0xff`, 5 s per arm, ~20 cm separation. `io errors after bring-up: 0` on
every run reported here.

## Result

```
  arm  configuration                 fps     Mbit/s    busy%
  A    broadcast,       No Ack      2018      22.60    58.2
  B    ucast nobody,    Normal        22       0.24    16.4
  C    ucast nobody,    No Ack        41       0.46    11.5
  D    ucast PEER,      Normal        22       0.25    16.4
  E    ucast PEER,      No Ack        56       0.63     7.1
  F    ucast PEER, ownSA Normal       22       0.25    16.5
  G    broadcast,  ownSA No Ack     2931      32.83    50.1
  V    ucast PEER, Normal, RX up   109 sent, 51 ACKs to our TA, 958 frames seen
```

**Arm V is the load-bearing one.** Without it the FAIL verdict would be
unfalsifiable — an unarmed or off-channel peer produces exactly the same number
as a MAC whose cliff genuinely survives being answered. Arm V repeats arm D
with the receiver up and counts 802.11 ACKs whose addr1 is our own addr2:
**51 ACKs against 109 frames sent.** The peer was answering, and the rate did
not move.

**Arms F and G kill the other candidate confound.** Every published measurement
of this cliff, and arms A–E, transmit from an invented addr2 that is not the
port identity. A real station transmits from its own address. Arm F does that —
`40:a5:ef:5a:32:f8` as addr2, unicast to the ACKing peer — and gets 22 fps,
identical to every other unicast arm. Arm G is its control: own SA, broadcast,
2931 fps, full ceiling. So the SA is not the variable either.

The only variable that moves the number is **addr1 broadcast vs unicast**. The
QoS Ack Policy is a weak second-order effect (22 → 41–56 fps, ~2×, still ~50×
below the ceiling). The ACK itself does nothing.

### Rig validation

- Arm A reproduced the published figure exactly on one run — **3043 fps /
  34.08 Mbit/s** against `docs/mt7612u.md`'s 3037 fps / 34.01 Mbit/s.
- The cliff ratio reproduced at 93–140× across runs, against a published ~40×.
- The peer's health was checked independently, because a burned PA would have
  produced this same FAIL for an unrelated reason. Peer RX: it logged 185
  frames at `len 1404` (our 1400-byte MPDU + FCS) at RSSI −18 dBm, so our
  frames reach it. Peer TX: the MT7612U received **12879 OFDM frames** from the
  8812AU running `txdemo` over 30 s. Both directions of the link are healthy.

### An adversarial note on the ceiling

Arm A is noisy — 2018, 2161, 3042, 3043 fps across four runs of the identical
configuration, a 50% spread. Ambient occupancy on ch149 and the USB 2.0 link
are the likely causes and neither was controlled. The conclusion survives it
only because the unicast arms are the opposite of noisy: 22 / 41 / 56 fps,
stable to ±1 across every run and every SA. Taking the *worst* ceiling still
leaves a 92× gap.

## What this does and does not establish

It establishes that **devourer cannot currently transmit unicast on the
MT7612U at a usable rate**, and that no host-side lever tried so far — ACK
policy, txwi ACK-REQ, WCID index, source address, or an actually-answering
peer — changes that.

It does **not** establish that the silicon cannot. That distinction decides
the whole project, and the evidence points away from the silicon:

- `mt76x2u` is a shipping in-tree Linux driver for this exact part. A MediaTek
  MT7612U in a normal station or AP role moves unicast traffic at hundreds of
  Mbit/s every day. A 22 fps unicast ceiling is not a property this hardware
  has.
- 22 fps is 45 ms per frame. That is not an ACK timeout (tens of µs) and it is
  not plausibly a retry ladder at this frame rate. It has the shape of a
  host-side stall — a ring slot not being reclaimed, or a completion waited on
  with a timeout — far more than a MAC behaviour.
- `busy%` corroborates: arm D holds the channel busy 16% while delivering
  22 fps. A MAC hammering retries would show far more; a MAC *waiting* shows
  about this.

So the most probable reading is that **this is a defect in devourer's MT7612U
TX path, not a limitation of the part** — and `docs/mt7612u.md`'s framing ("the
MAC arms an ACK timeout for a peer that never answers") is an inference that
this measurement now contradicts, because the peer answered and nothing
changed.

## Verdict and what happens next

**Gate: FAIL — and the failure is not where the plan expected it.** Station
mode on MT7612U is blocked, but by a driver defect that is worth fixing on its
own merits rather than by a hardware ceiling that would have ended the project.

The gate cannot close — in either direction — until one more measurement
exists, and it is the decisive one:

**Phase 0b — the kernel A/B.** Bind `mt76x2u` to the same adapter and measure
unicast TX throughput through the kernel driver on the same channel against the
same peer. This is the control that arms A–G lack, and the repo's standing rule
about adversarial counterparts demands it.

- If the kernel driver also collapses on unicast injection, the finding is
  about the part and `docs/mt7612u.md` stands. Station mode on MT7612U stops
  here.
- If the kernel driver does not, the finding is a devourer TX-path bug. It gets
  its own issue, and station mode resumes behind it.

Until 0b runs, the honest status of Phase 0 is **blocked on a driver defect of
unknown origin**, not "the part cannot do it".

### Follow-ups this run generated regardless of 0b

- `mt7612u_link_stats()` reads the die temperature over the MCU, and under
  sustained TX that read times out — `mcu command timed out waiting for
  response` and `drained 2 stale MCU replies before cmd 31` appeared on every
  arm boundary. Harmless here, but it means a caller polling link stats during
  heavy TX gets MCU errors as a matter of course, which is worth either fixing
  or documenting at the call.
- `bringup rx` reports 0 frames where `bringup arx` reports 12879 in the same
  conditions. The gate does warn that it blocks the only EP 4 drainer, but it
  still prints `GATE F: FAIL - no frames received`, which reads as a hardware
  verdict. It should refuse to grade instead.
