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

`filtr=00015f97` in every arm, and **every write is now read back** — the gap
the scope document asked about from the start. Arm C's slot 0 reads
`hi=00009111` (the AP's `…:11:91`) and arm F's slot 1 reads `hi=000001ad` (the
wrong `…:ad:01`), so the arms programmed what they claimed to.

The read-back also settled a question that would otherwise have undermined all
of this: **BIT(16) of the APC high register was CLEAR in every arm.** Upstream
mt76 calls that `MT_MAC_APC_BSSID0_H_EN` and this tree has never defined it,
so "a wrong BSSID changes nothing" could have meant "nothing was reading the
BSSID". That is closed below — see arm E.

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
| **A** — DUT receiving, **nothing armed** | 887 | **100.0%** | **0.10** |
| B — destination nobody holds | 272 | 0.0% | 12.00 |
| C — DUT not running | 262 | 0.0% | 12.00 |
| **D** — DUT receiving, `MT_AUTO_RSP_EN` **cleared** | 273 | 0.0% | 12.00 |
| **E** — DUT receiving, **wrong BSSID in an ENABLED APC slot** | 863 | **100.0%** | **0.14** |

**A against B and C** establishes the claim: with nothing armed at all, this
MAC acknowledges unicast addressed to its own address. B holds the
transmitter, rate, channel and timing fixed and changes only the destination,
so it also shows the match is address-specific rather than promiscuous
answering. C removes the DUT entirely.

**D was NOT single-variable as run, and the table above predates the fix.**
Arm A ran `bringup arx`, which installs the **monitor** filter at the top of
`gate_arx`; arm D ran `norsp`, which leaves the managed one. So the comparison
varied the receive filter *and* the init path as well as the bit under test,
in a section that called it single-variable. Same defect class as the R5 gate
overwriting its own filter, caught in review rather than on the bench.

The harness is fixed — both arms now run `norsp`, which takes the bit as an
argument, so they share one code path and differ by exactly one bit — **but
the numbers above have not been re-taken under it.** Treat D as indicative,
not as the single-variable result it is described as, until a re-run.

What survives regardless: **arm E** ran the managed filter with the bit SET
and reads 100%, and **arm D** ran the managed filter with it CLEAR and reads
0%. Those two are both managed, so the filter is not what separates them —
they differ in the bit *and* in the BSSID programming, which E itself shows is
inert. So the conclusion that `MT_AUTO_RSP_EN` gates acknowledgement is
supported; the claim that any one arm pair isolated it is not.

**E closes R5.** It programs a deliberately wrong BSSID into both
`MT_MAC_BSSID` and the derived APC slot, sets mt76's per-slot enable bit,
verifies the bit stuck, and then receives — and acknowledgement is unchanged
at 100%. So the BSSID plane does not gate a station on this part even when its
enable is set, and R5's null result is not an artefact of that bit being
clear. The gate refuses rather than reports if the bit will not stay set: an
unsettable bit is not evidence about anything.

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

## The uplink — what this station transmits is acknowledged

The other half of `AdapterCaps::station_mode_ok`'s bar. Everything above
measures frames sent *to* the DUT; this measures frames sent *by* it.

Instrument: the DUT's own `MT_TX_STAT_FIFO` via `bringup txs`, which reports
the MAC's per-MPDU retry count. The peer is a Realtek adapter running `rxdemo`
with `DEVOURER_ACK_RESPONDER` armed — the same responder
`tests/ack_txreport_matrix.sh` uses.

`sudo tests/mt7612u_sta_uplink.sh`

| arm | acknowledged | mean retries | max |
|---|---|---|---|
| **A** — peer answers for the address we transmit to | **200/200** | **0.0** | 1 |
| B — peer answers for a *different* address (control) | 0/200 | 16.0 | 16 |

Read from the gate's **"MAC receiver ON"** table. That gate prints its arms
twice, receiver off and receiver on, which is Phase 0's result built in: with
the receiver off the MAC cannot hear an ACK, so every unicast arm runs its
ladder to exhaustion regardless of what the peer does. A station runs with its
receiver on. Reading the wrong table reported UNSETTLED for both arms and
threw away a clean measurement.

**On arm B and the UNSETTLED marker.** The gate flags an arm when fewer status
entries land than frames were sent, because it cannot then guarantee each
entry belongs to the arm it is printed under. The failing control trips that
*by construction*: with nothing acknowledging, every frame runs the full
16-retry ladder, the MAC is about two orders of magnitude slower per frame,
and the 16-slot ring cannot keep up. A control that always reads UNSETTLED is
not a control.

It is accepted here, and only here, for a stated reason: misattributed entries
would come from the neighbouring arms, which in the same table run at 200/200
and zero retries — so contamination can only make a failing arm look *better*.
Arm B's 0/200 at 16.0 retries is therefore a floor, and a floor is all a
control needs to be. The harness still refuses an UNSETTLED arm that *claims
success*.

## What is not established

- **The R6 table needs a re-run** under the corrected harness — see the note
  under arm D. The conclusion is supported by the D/E pair; the single-
  variable claim for A/D is withdrawn.
- **THE LIBRARY'S OWN STATION PATH DOES NOT USE THE MANAGED FILTER.**
  `Mt7612uRadio::StartRxLoop` calls `mt7612u_set_monitor_rx()`
  unconditionally after `mt7612u_start()`, so a station driven through
  `IRadio` runs PROMISCUOUS. Every "managed station" result here therefore
  describes a configuration this library does not currently enter, and the
  "moving `MT_MAC_ADDR` makes a station deaf" half of the rationale is a
  property of the managed filter specifically. Under the monitor filter the
  station would still RECEIVE with the port identity moved — it would simply
  stop acknowledging. The prohibition stands either way; the reason differs
  by filter, and Phase 3 has to decide which filter a station should run.
- **No cell drove `SetStationIdentity` itself.** The seam writes no register,
  so the measured hardware state is the state a successful arm leaves behind
  — but the literal "arm the seam, then measure" path is unexercised.
- **Everything is an unassociated station** receiving traffic it did not
  negotiate. Power save, TIM parsing, cross-BSS duplicate detection and
  hardware key lookup are untested; Phase 3 should expect to revisit this.
- One AP, one DUT, one channel, near field, and no soak.
