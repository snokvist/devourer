# What an MT7612U station actually needs programmed

Phase 2's measurement record. `docs/station-mode-scope.md` raised two risks
against the backend half of `SetStationIdentity` and neither had been
measured — R5 (what the APC BSSID slot does for a managed station) and R6
(`MT_MAC_ADDR` gaining a third co-owner). Both are measured here.

Reproduce with a real AP on the channel:

```sh
# the RTL8812AU runs hostapd; the MT7612U is the device under test
sudo tests/mt7612u_sta_identity.sh
```

Or the gates directly, against any AP:

```sh
sudo build/mt7612uprobe sta    6 12 <ap-bssid>   # R5: what gates receive
sudo build/mt7612uprobe staack 6 25 <ap-bssid>   # R6: do we auto-ACK
```

Rig: AP = RTL8812AU `0bda:8812` on the in-tree rtw88 driver running
hostapd 2.10, BSSID `02:42:75:05:d6:aa` (deliberately locally administered —
see below). DUT = MT7612U `0e8d:7612`, own MAC `40:a5:ef:5a:32:f8`, driven by
`build/mt7612uprobe`. Channel 6, near field.

## R5 — the BSSID registers do not gate a station's receive. At all.

Six arms over the two places a BSSID can live on this part: `MT_MAC_BSSID`
(the MBSS base that the APC slot index is derived from) and the
`MT_MAC_APC_BSSID` slot table. **No arm touches `MT_MAC_ADDR`.**

The AP's BSSID is locally administered on purpose. mt76 derives the slot as
`idx = 1 + (((mbss_base[0] ^ addr[0]) >> 2) & 7)` for a locally administered
address and **0 otherwise** — so with a factory BSSID the "slot 0" and
"derived slot" arms would have been the same test. Here the rule yields
slot 1, and slot 0 and slot 1 are both exercised.

Unicast at the DUT is injected from a monitor vif on the AP's own phy,
because an unassociated station is sent no unicast by hostapd and every arm
otherwise reads `to_us = 0` — i.e. the first run of this gate measured
broadcast reception only and could not have answered the question.

| arm | configuration | slot | from_bss | beacons | unicast to us |
|---|---|---|---|---|---|
| A | init only, nothing programmed | 1 | 3989 | 105 | 3884 |
| B | `MT_MAC_BSSID` = AP | 1 | 3654 | 105 | 3547 |
| C | APC slot 0 = AP | 1 | 3647 | 112 | 3533 |
| D | APC slot (mt76 rule) = AP | 1 | 3662 | 115 | 3545 |
| E | `MT_MAC_BSSID` + derived slot = AP | 1 | 3657 | 117 | 3540 |
| **F** | **both programmed WRONG** | 1 | 3657 | 117 | **3538** |

Register read-backs confirm the writes landed (arm B/E `dw0=05754202
dw1=003faad6` is `02:42:75:05:d6:aa` little-endian; arm F `dw0=de000002`
is the wrong address).

**Arm F is the finding.** A deliberately wrong BSSID in both registers
receives 3538 unicast frames against 3884 with nothing programmed — the same
number, within the spread the arms show among themselves. Programming the
BSSID correctly, incorrectly, or not at all makes no difference to what a
managed station receives, broadcast or unicast.

This is the **opposite** of the AP-side result, where a wrong APC slot was
silent but fatal ("beacons perfectly, acknowledges nobody" —
`docs/mt7612u-ap-mode.md` finding 2). Note what that phrasing actually says:
the AP-side failure was in **acknowledgement**, not reception. So the two
results do not contradict each other, and the AP finding must not be
extrapolated to a station's receive path — which is exactly the extrapolation
review round 1 caught the scope document making.

Arm A is ~9% above the others on both counts. That is the injector warming up
in the first arm, not a signal; nothing here rests on a 9% difference.

## R6 — this MAC auto-ACKs with nothing armed, and moving `MT_MAC_ADDR` destroys that

The scope document asserted, as "good news", that because the auto-response
engine matches address 1 against `MT_MAC_ADDR` and `MT_AUTO_RSP_EN` is on
from init, an MT7612U station auto-ACKs the AP's unicast with no call at all.
That was read off the registers and never measured.

Measuring it needs someone to observe the ACK, and the obvious approaches do
not work. **What failed, recorded so it is not retried:** injecting at
ourselves from a monitor vif on the AP's phy and capturing ACKs there
produced **zero in both the DUT-present and DUT-absent arms**. The control is
the only reason that non-result was not written up as "the station does not
ACK" — a radio cannot hear an ACK to its own transmission, and
monitor-injected frames default to no-ack in mac80211 so they never solicited
one.

What works needs no second observer. Send the AP a **directed probe request**
from our own address; it answers with a unicast probe response through its
normal transmit path, with retries. Then count the copies:

- if we ACK it, the AP is done — one copy, FC Retry clear;
- if we do not, its MAC retransmits until its limit — the same response
  again with FC Retry **set**.

`retried` is the signal. It is the sound form of the auto-ACK test, and the
same "count the retried copies" that `docs/mt7612u-ap-mode.md`'s Gate B row
used and the AP on-air script never did.

| arm | probe reqs | responses to us | retried | |
|---|---|---|---|---|
| A — nothing armed | 125 | 126 | 1 | **0.8%** |
| B — `MT_MAC_ADDR` retargeted away | 125 | 23082 | 22611 | **98.0%** |

**Arm B is the control, and it is what makes arm A quotable.** Without it,
"few retries" might simply be what this AP always does, and the gate could
not have failed. Arm B reaches the failing state by doing precisely what R6
warns against — `mt7612u_set_ack_responder()` retargets `MT_MAC_ADDR` to a
foreign address, so the engine stops matching our own.

Two things follow:

1. **R6's claim is true, measured.** With nothing armed at all, this MAC
   acknowledges unicast addressed to its own address.
2. **The R6 hazard is real and large, demonstrated rather than asserted.**
   `SetAckResponder(bssid)` on a station would move `MT_MAC_ADDR` to the AP's
   address and take acknowledgement from 0.8% retried to 98%. That is the
   reason the seam is `SetStationIdentity` and not a reuse of
   `SetAckResponder`.

### Arm B changes two things, and the raw count says so

Arm B received **23082** responses to 125 probe requests. An earlier revision
of this document called that "the AP's retry ladder running to exhaustion,
~184 copies per response". That cannot be right: no 802.11 retry limit is
anywhere near 184.

Moving `MT_MAC_ADDR` has a second consequence, and this tree already documents
it — `src/mt7612u/tools/bringup.cpp` notes that this MAC matches an **inbound**
ACK's address 1 against `MT_MAC_ADDR` as well. So in arm B:

1. the AP's ACKs to **our** probe requests are rejected by our own MAC, so our
   MAC retransmits each request through its ladder; and
2. each copy that reaches the AP draws a fresh probe response, each of which we
   then fail to acknowledge, so the AP retransmits that too.

Two ladders multiplying, which is the right order of magnitude for 23082 from
125. **That is inference from the arithmetic plus a documented property of this
MAC, not a separate measurement**, and it is written here as inference.

It matters for how much arm B is allowed to prove. The arm is not a clean
single-variable change — it stops us acknowledging *and* stops us accepting
acknowledgement. But the **retried fraction** is unharmed in the direction
that counts: our own retransmissions make the AP emit *fresh* responses, which
arrive with Retry **clear** and so push the fraction DOWN. The measured 98% is
therefore conservative, and the conclusion — that we stopped acknowledging —
is the one reading the fraction supports.

## What this means for `SetStationIdentity` on MT7612U

On the evidence above, and stated as what was measured rather than what was
expected:

- It **must not** move `MT_MAC_ADDR`. That is the one action shown to break a
  station, and it is the failure mode with the largest measured effect in
  this document.
- It does **not** need to program the BSSID for receive to work, and a wrong
  value there is harmless for receive.
- Auto-ACK needs no call.

So the honest MT7612U implementation is small, and its job is mostly to
**refuse** things: verify `own` is the port identity the MAC was brought up
with and fail if it is not, rather than "fixing" it by writing the register.

**What is NOT established.** Every number here is from an unassociated
station receiving traffic it did not negotiate. Nothing here says the BSSID
registers are irrelevant to an *associated* station — only that they do not
gate reception or acknowledgement of frames addressed to us. Anything that
depends on the hardware knowing which BSS we belong to (power save, TIM
parsing, duplicate detection across BSSIDs, hardware key lookup by BSS) is
untested, and Phase 3 should expect to revisit this. One AP, one DUT, one
channel, near field, no repetition beyond what the tables show.
