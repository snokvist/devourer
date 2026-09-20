# What an MT7612U station actually needs programmed

Phase 2's measurement record. `docs/station-mode-scope.md` raised two risks
against the backend half of `SetStationIdentity` — R5 (what the APC BSSID slot
does for a managed station) and R6 (`MT_MAC_ADDR` gaining a third co-owner).

**Read the retraction section first if you saw an earlier revision of this
file.** Its headline numbers were taken with the wrong receive filter and are
withdrawn. The conclusions survived re-measurement; two of the three arguments
for them did not.

```sh
sudo AP_SYSFS=6-1 DUT_SYSFS=7-1 CH=6 tests/mt7612u_sta_identity.sh
```

Rig: AP = RTL8812AU on the in-tree rtw88 driver running hostapd 2.10, BSSID
`02:42:75:05:d6:aa` (locally administered on purpose — see R5). DUT = MT7612U,
own MAC `40:a5:ef:5a:32:f8`. Channel 6, near field, 20 s per arm.

## The retraction, and what caused it

Every arm of the first run called `mt7612u_set_monitor_rx()` under a comment
reading `/* managed filter, not monitor */`. That is exactly inverted. The
function writes `MT_RX_FILTR_CFG = PHY_ERR|CRC_ERR` and nothing else — its own
doc says it "clears everything except the two error classes" — so it turns
every address and BSS drop bit **off**.

So the gate replaced the configuration under test with its opposite, one
statement before the dwell. Six identical arms were guaranteed before any
frame arrived, and the null result was a tautology. It is the Phase 0 defect
in a new costume, two lines below a comment congratulating the gate for not
repeating Phase 0.

A second error fed it. Review round 1 had "corrected" the scope document to say
that because bit 3 (`OTHER_BSS`) is clear in the managed filter `0x00015f97`,
other-BSS frames are accepted and a wrong BSSID cannot deafen a station. But
bit **2** (`PROMISC`) is SET in that value, and in mt76 bit 2 is the one mapped
to `FIF_OTHER_BSS`. So round 1 replaced a wrong mechanism with a second wrong
mechanism, and three documents carried it as settled. Because the reasoning had
already concluded the filter was not in play, nobody noticed the gate
overwriting it.

Everything below is re-measured with `MT_RX_FILTR_CFG = 0x00015f97` in force,
which the gate now prints per arm and flags if it is not.

## R5 — the BSSID registers do not gate a managed station's receive

Six arms over the two places a BSSID can live: `MT_MAC_BSSID` (the MBSS base
the APC slot index derives from) and the `MT_MAC_APC_BSSID` slot table. **No
arm touches `MT_MAC_ADDR`.** Both register families are reset to their init
state between arms — an earlier version reset only the MBSS base, so arm F's
"both programmed WRONG" still had arm C's correct BSSID live in slot 0.

The AP's BSSID is locally administered so that mt76's rule
`idx = 1 + (((mbss_base[0] ^ addr[0]) >> 2) & 7)` yields a non-zero slot; with
a factory BSSID it returns 0 and the "slot 0" and "derived slot" arms would be
the same test. Unicast at the DUT comes from a monitor vif on the AP's own phy,
because hostapd sends an unassociated station none and every arm would
otherwise read `to_us = 0`.

| arm | configuration | slot | from_bss | beacons | unicast to us |
|---|---|---|---|---|---|
| A | init only, nothing programmed | 1 | 6445 | 195 | 6250 |
| B | `MT_MAC_BSSID` = AP | 1 | 6071 | 194 | 5877 |
| C | APC slot 0 = AP | 1 | 6072 | 194 | 5878 |
| D | APC slot (mt76 rule) = AP | 1 | 6083 | 192 | 5891 |
| E | `MT_MAC_BSSID` + derived slot = AP | 1 | 6083 | 195 | 5888 |
| **F** | **both programmed WRONG** | 1 | 6073 | 196 | **5877** |

`filtr=00015f97` in every arm. Register read-backs confirm the `MT_MAC_BSSID`
writes land; the APC writes are **not** read back, which is a gap the scope
document specifically asked for and this gate still does not close.

**Arm F is the finding.** A deliberately wrong BSSID in both registers receives
5877 unicast frames against arm A's 6250 with nothing programmed — flat, and
arm A's ~6% excess is the first-arm-of-the-run pattern that appears in every
run of this gate rather than a response to configuration.

This is the opposite of the AP-side result, where a wrong APC slot was silent
but fatal. Read that finding's own words — "beacons perfectly, **acknowledges**
nobody" — and the AP-side failure was in acknowledgement, not reception. The
two do not conflict, and the AP result must not be extrapolated to a station's
receive path.

## R6 — NOT established. The method is void on this rig.

The scope document asserted, as "good news", that this MAC auto-ACKs a
station's unicast with no call at all, because the auto-response engine matches
address 1 against `MT_MAC_ADDR` and `MT_AUTO_RSP_EN` is on from init. That was
read off the registers. **It is still not measured.**

The approach was to make the AP answer a directed probe request through its
normal transmit path and count retried copies: if we acknowledge, one copy with
Retry clear; if we do not, retransmissions with Retry set.

| arm | probe reqs | responses to us | retried | |
|---|---|---|---|---|
| A — nothing armed | 100 | 103 | 0 | 0.0% |
| B — **`MT_AUTO_RSP_EN` cleared** (control) | 100 | 100 | 1 | **1.0%** |
| C — `MT_MAC_ADDR` retargeted | 100 | **0** | — | no responses |

**Arm B is a single-variable control and it did not move.** Reception is
identical to arm A — same port identity, same filter, responses still addressed
to us — and the only change is that the answering engine is disabled. The
retried fraction stayed flat. So either this MAC acknowledges by a path
`MT_AUTO_RSP_EN` does not gate, or **retried copies do not track
acknowledgement on this rig at all** — the likeliest reading being that this AP
does not retransmit an unacknowledged probe response. Either way the method
cannot fail, so arm A's 0.0% proves nothing and the gate reports INCONCLUSIVE.

### What is withdrawn

An earlier revision reported arm A at 0.8% against a control at **98.0%** over
23082 responses, and called that proof. That control was the `MT_MAC_ADDR`
retarget, run with the monitor filter installed by mistake — promiscuous, so
every retransmission on the channel was visible. Under the managed filter the
same arm (C above) receives **nothing at all**. Both the 98% and the two-ladder
arithmetic offered to explain 23082 are withdrawn.

### What arm C does establish, and it is cleaner than what R6 asked for

Under the managed filter, moving `MT_MAC_ADDR` takes reception from 103
responses to **zero**. The port identity gates what a station *receives*, not
merely whether it acknowledges — which is a larger failure than the one the
seam was designed around, and it is measured, single-variable, and
unambiguous.

So `SetStationIdentity`'s refusal to move `MT_MAC_ADDR` is fully justified —
just not by the argument that was originally offered for it.

## What this means for `SetStationIdentity` on MT7612U

- It **must not** move `MT_MAC_ADDR`. Established by arm C above (reception),
  not by the auto-ACK story (unmeasured).
- It does **not** need to program the BSSID, and a wrong value there is
  harmless for receive. Established by R5.
- Whether this MAC auto-ACKs is **open**. Nothing in the implementation depends
  on the answer — it makes no call either way — but `AdapterCaps::station_mode_ok`
  does, and stays false.

## What is not established

- **Auto-ACK, either direction.** Needs an instrument that survives the managed
  filter: the chip's own `MT_TX_STAT_FIFO` retry count for a station's uplink
  (`bringup txs`) is the obvious candidate and has not been pointed at this.
- **The APC writes are never read back.** Arm F could be writing a slot the
  hardware does not consult and the table would look the same.
- **A per-slot enable bit.** Upstream mt76 carries `MT_MAC_APC_BSSID0_H_EN` at
  BIT(16) of the slot-0 high register; this tree defines no such bit and none
  of arms C–F sets one. If per-slot enable is real here, those arms may never
  have enabled the slot they programmed.
- **Everything is an unassociated station** receiving traffic it did not
  negotiate. Power save, TIM parsing, cross-BSS duplicate detection and
  hardware key lookup are untested; Phase 3 should expect to revisit this.
- One AP, one DUT, one channel, near field.
