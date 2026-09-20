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

## R6 — ANSWERED: this MAC auto-ACKs with nothing armed, and `MT_AUTO_RSP_EN` is the gate

The scope document asserted this from a register reading. It is now measured,
on the third method — the first two are recorded below because both produced
confident-looking non-results.

**The instrument.** Ask the *transmitter*, which is the only party that knows
whether its frame was acknowledged. A Realtek adapter under devourer injects
unicast QoS-Data at the DUT and reads its own per-frame CCX `tx.report`:
`retries≈0` means the DUT answered, `retries` pinned at the descriptor limit
means it did not. Same instrument as `tests/ack_txreport_matrix.sh`, pointed
the other way round — there the Realtek part is the responder, here it is the
witness. Peer: RTL8812CU (Jaguar3 drains C2H off its coex runtime, so reports
arrive without further arrangement). Retry limit pinned at 12 so a no-ACK
outcome is visible per frame.

`sudo tests/mt7612u_sta_autoack.sh`

| arm | reports | ok | retries (mean) |
|---|---|---|---|
| **A** — DUT receiving, **nothing armed** | 876 | **100.0%** | **0.06** |
| B — destination nobody holds | 272 | 0.0% | 12.00 |
| C — DUT not running | 269 | 0.0% | 12.00 |
| **D** — DUT receiving, `MT_AUTO_RSP_EN` **cleared** | 261 | 0.0% | 12.00 |

**A against B and C** establishes the claim: with nothing armed at all, this
MAC acknowledges unicast addressed to its own address. B holds the
transmitter, rate, channel and timing fixed and changes only the destination,
so it also shows the match is address-specific rather than promiscuous
answering. C removes the DUT entirely.

**D is single-variable and it settles the mechanism.** Same receiver, same
port identity, same managed filter, one bit different — and acknowledgement
stops dead. So `MT_AUTO_RSP_EN` *is* the gate on this part, which retroactively
justifies `SetStationIdentity` refusing to arm when that bit is clear: a branch
that shipped on an assumption now has a measurement behind it.

### The two methods that failed first

Recorded because both looked like results, and one was briefly written up as
one.

1. **Capture the DUT's ACKs on a monitor vif on the peer's own phy.** Zero in
   both the DUT-present and DUT-absent arms. A radio cannot hear an ACK to its
   own transmission, and mac80211 marks injected frames no-ack so they never
   solicited one. Only the DUT-absent control stopped this being written up as
   "the station does not acknowledge".
2. **Make hostapd answer a directed probe request and count retried copies.**
   Arm A gave 0.8% retried and an earlier revision of this document reported
   it against a 98% control as proof. That control retargeted `MT_MAC_ADDR`
   *and* ran with the monitor filter installed by mistake — under the managed
   filter the same arm receives nothing at all. Replacing it with the clean
   single-variable control (clear `MT_AUTO_RSP_EN`, hold reception constant)
   produced **no movement**, because this AP does not retransmit an
   unacknowledged probe response. The method could not fail. Both the 98% and
   the two-ladder arithmetic offered to explain its 23082 responses are
   withdrawn.

The common flaw: both asked the DUT, or a device that could not see the
answer. The third method asks the transmitter.

### And the separate thing arm C of the R5 gate established

Under the managed filter, moving `MT_MAC_ADDR` takes the DUT's *reception* of
the AP's unicast from 103 frames to **zero**. The port identity gates what a
station receives, not merely whether it acknowledges. So there are now two
independent reasons `SetStationIdentity` must not move that register, and the
reception one is the larger failure.

## What this means for `SetStationIdentity` on MT7612U

- It **must not** move `MT_MAC_ADDR`. Established by arm C above (reception),
  not by the auto-ACK story (unmeasured).
- It does **not** need to program the BSSID, and a wrong value there is
  harmless for receive. Established by R5.
- It **must** refuse when `MT_AUTO_RSP_EN` is clear. Measured: clearing that
  bit stops acknowledgement dead (arm D), so the refusal is not defensive
  programming.
- Auto-ACK needs no call: with nothing armed, 100% of the peer's frames are
  acknowledged at 0.06 mean retries.

## What is not established

- **The station's own uplink.** Everything above measures frames sent *to* the
  DUT. Whether an AP acknowledges what this station *transmits* is a different
  question, and `AdapterCaps::station_mode_ok`'s bar asks for it;
  `MT_TX_STAT_FIFO` and `bringup txs` are the instrument and have not been
  pointed at it.
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
