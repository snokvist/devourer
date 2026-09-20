# Station mode — the plan, its gates, and the review ledger

The technical scope is `docs/station-mode-scope.md`. This file is the **living
plan**: what the phases are, what each gate demands, and which adversarial
reviews each gate has actually survived. It is updated as work lands, not
written once.

## The end goal, stated so a gate can be judged against it

A **harness** that fits devourer's existing structure and lets devourer join a
real access point — scan, authenticate, associate, WPA2-PSK 4-way as the
supplicant, encrypted data plane. The MT7612U is the guinea pig: the first and
only backend implemented now, chosen because AP mode is already proven there
end to end.

The test of "fits the structure" is not a design review, it is a property:
**the station harness must contain no backend branch.** `tests/ap_responder.cpp`
and `tests/ap_wpa2.cpp` already meet it — they run unchanged on MT7612U and on
three Realtek generations. A Realtek station arm must therefore be *only* an
`IRadio` method implementation, never a change to the harness. If a later
backend forces a harness edit, the seam was wrong and the gate that passed it
failed.

## The gate discipline

No phase is complete on the author's say-so. Each gate closes only when:

1. The phase's own acceptance evidence exists (measurement, ctest cell, or
   on-air cell — stated per phase below).
2. **At least two adversarial reviews** have been run against the phase's
   output by independent reviewers, with the findings recorded in the ledger.
3. Every finding is resolved: fixed, or explicitly rejected with a reason
   written down. "Noted" is not resolved.
4. The plan document is updated with what the phase actually found, including
   anything that contradicts an earlier phase.

Reviewers are independent in the sense that matters here: a different model or
a different agent, given the artifact and the repo, asked to attack it rather
than confirm it. A review that returns no findings is evidence about the
reviewer, not about the artifact — rerun it with a sharper prompt.

Standing rules inherited from the repo that a reviewer should check against:
the library reads no environment; two-plane logging; no key material in logs;
`std::thread` not `std::jthread` (#426); MSVC and mingw are first-class; no
favourable measurement without its adversarial counterpart in the same breath.

## Review ledger

| Gate | Artifact | Reviewer | Verdict | Findings | Resolved |
|---|---|---|---|---|---|
| Scope | `docs/station-mode-scope.md` | Flash (`deepseek-v4.1-flash`) | changes required | 17 (F1–F17) | yes — see below |
| Phase 0 | `docs/station-mode-phase0.md` + `gate_ucast` | Flash (`deepseek-v4.1-flash`) | changes required | 9 (F1–F9) | yes |
| Phase 0 | `docs/station-mode-phase0.md` + `gate_ucast` | Opus subagent | **verdict overturned** | 9 (S1–S9) | yes |

### Round 1 — Flash, 2026-09-20

Seventeen findings. Five were verified against the code by hand before being
accepted, because a review that is confidently wrong is worse than no review:

- **F1 accepted (critical).** R5's mechanism was wrong. The managed filter
  `0x00015f97` has `MT_RX_FILTR_CFG_OTHER_BSS` (bit 3) **clear** — decoded the
  constant bit by bit; other-BSS frames are accepted, not dropped, and
  `src/mt7612u/tools/bringup.cpp` says so in the tree. The claim "a wrong APC
  slot is a station that hears nothing" had no mechanism. R5 is now an open
  question rather than an asserted failure mode, and Phase 2 must measure it
  before writing a regcheck against it.
- **F2 accepted (high).** `Mt7612uRadio::StartRxLoop` writes the monitor filter
  unconditionally at `src/mt7612u/Mt7612uRadio.cpp:410-412`, deliberately,
  after `mt7612u_start()` has written the managed value. A
  `SetStationIdentity` called before `StartRxLoop` would be silently
  overwritten. The contract now carries the ordering, and
  `ClearStationIdentity` returns `bool` instead of `void` so a failed restore
  is reportable.
- **F3 accepted (high).** `MT_TXWI_ACK_CTL_NSEQ` — hardware sequence assignment
  — is set for beacons only (`src/mt7612u/tx.cpp:169-172`), and both AP
  harnesses hardcode the sequence bytes to zero. A station's data plane needs a
  sequence counter or it feeds the AP's duplicate detector. Added to the work
  list.
- **F5 accepted (correction of a sloppy absolute).** `std::jthread` is in the
  tree today at `src/jaguar1/RtlJaguarDevice.cpp:1670,1683` and
  `src/rtl8733b/Rtl8733bDevice.cpp:189,193`. The rule is "do not add a third
  site", not "the tree has none".
- **F17 accepted.** `ap_responder.cpp` links no crypto; only `ap_wpa2.cpp`
  does. Several other small corrections applied.

F4, F6, F8–F16 accepted and folded in: peer WCID binding, the honest position
on the "two backends" argument, ADDBA-decline being real work, station-side
ARQ being unported, narrowband APs being unjoinable, capability/width
negotiation, host-timed beacon supervision, `Ccmp` riding `CryptoOps` rather
than OpenSSL, and the benchmark artifacts not being in the tree.

Nothing was rejected. The review found a genuine mechanism error in a risk the
document had marked as understood, which is the outcome the gate discipline
exists to produce.

## Phase 0 — the gate that can end the project

**Question.** `docs/mt7612u.md` measures a 40× unicast TX cliff on this part:
3037 fps broadcast against **75 fps unicast**, attributed to the MAC arming an
ACK timeout for a peer that never answers. A station's entire data plane is
unicast to the AP. Does the cliff survive a peer that actually ACKs?

**Why it has to come first.** If it does survive, MT7612U station mode is a
management-plane demonstration and not a link, and the right move is to
re-scope or to make Realtek the guinea pig instead. Every later phase is
wasted work until this is answered, and it is answerable in a day with
hardware already on the bench.

**What the existing measurement actually did** — this matters, because it
narrows the question. `src/mt7612u/tools/bringup.cpp` (`gate_ampdu`) airs the
unicast arms with **QoS Ack Policy = No Ack** (`frame[24] = 0x20`) *and*
`rate.no_ack = 1`, and the cliff is present anyway. `docs/mt7612u.md` records
the same: clearing `txwi.ack_ctl`'s REQ bit does not prevent it, and neither
does the QoS No Ack policy. So the two obvious host-side levers are already
known not to help, and the only untested variable left is **whether an ACK
actually comes back**.

The arithmetic supports testing it: 75 fps is ~13.3 ms per frame. A single ACK
timeout is tens of microseconds. 13 ms is a retry ladder with backoff running
to exhaustion — which is exactly what should collapse to one ACK time when the
peer answers on the first attempt.

**Design.** Three arms, one session, one channel, one rate, one frame size, so
the comparison is internal and needs no cross-session calibration:

| arm | addr1 | ack policy | expectation if the hypothesis holds |
|---|---|---|---|
| A | broadcast | No Ack | the ceiling — reproduces ~3000 fps |
| B | unicast, nobody listening | Normal Ack | reproduces the cliff (~75 fps) |
| C | unicast, **to an ACKing peer** | Normal Ack | the question — near A means go |

Arm B is the control that proves the rig reproduces the published cliff before
arm C's number is allowed to mean anything. Arm C uses **Normal Ack**, not No
Ack, because that is what a real station transmits.

**Peer.** An RTL8812AU running `rxdemo` with `DEVOURER_ACK_RESPONDER=<mac>` —
devourer's own hardware ACK responder, on independent silicon. `docs/mt7612u.md`
already measured this responder path in the opposite direction (an MT7612U
answering an 8812AU stimulus with 3500+ ACKs), so the mechanism is known to
work; this run drives it the other way.

**Adversarial counterpart, required.** A devourer ACK responder is not a real
AP. If the peer arm passes, repeat against **hostapd on non-MediaTek silicon**
before the gate closes. If the two disagree, the gate fails and the
disagreement is the finding.

**Implementation.** `bringup ucast <chan> <secs> [peer-mac]`
(`src/mt7612u/tools/bringup.cpp`, `gate_ucast`). Five arms rather than three —
the design above was sharpened after reading what `gate_ampdu` actually did:

| arm | addr1 | ack policy | role |
|---|---|---|---|
| A | broadcast | No Ack | the ceiling |
| B | unicast, nobody listening | Normal Ack | the published cliff |
| C | unicast, nobody listening | No Ack | the published cliff, other policy |
| D | unicast, **to the ACKing peer** | Normal Ack | **the question** |
| E | unicast, to the ACKing peer | No Ack | separates the address from the ACK |

Same channel, same rate (HT MCS7 BW20), same 48-byte QoS frame, `wcid = 0xff`
throughout — the published table's exact configuration, so the numbers land on
one axis. The gate refuses to reach a verdict unless A/B reproduce at least a
5× cliff first, and prints INCONCLUSIVE rather than a verdict if arm D lands
between the two.

**Second question, same session.** `tx_retry_limit_ok` is false on MT7612U and
no test drives the hardware retry counter. Whether the MAC retransmits the
station's own frames when an ACK is *missed* is a separate requirement from
whether an answered ACK restores throughput, and Phase 0 should answer both
while the rig is up.

**Acceptance.** Arms B/C within noise of the published 75 fps (the rig
reproduces the known result), and arm D either near arm A (**go**) or near arm
B (**stop, re-scope**). A result between them is not a pass — it is a number
that needs a reason before the project spends anything on it.

**Status: RUN — verdict PASS, gate CLOSED.** Full record:
`docs/station-mode-phase0.md`.

The station configuration — own port identity as addr2, MAC receiver on,
armed peer, Normal Ack — runs at **2084 fps with 10406 ACKs against 10421
frames sent (99.9%)**, 71% of its matched own-SA broadcast control. The 40×
unicast cliff is real and is a property of **TX-only injection with the MAC
receiver disabled**, where no ACK can be consumed and every frame runs a
15-deep exponential-backoff ladder to exhaustion (~46 ms by arithmetic, 45.5 ms
measured). It does not apply to a station.

**This verdict is the reverse of the one this gate first reached**, and the
reversal is the clearest argument for the two-reviewer rule in this document.
The first run measured seven arms, every one of them with the MAC receiver
off, and concluded FAIL. The write-up then dismissed the retry ladder without
arithmetic and read `busy%` with the sign inverted. Review round 2 found the
disabled receiver, did the arithmetic, and named the missing arm. See the
appendix of `docs/station-mode-phase0.md`.

Phase 0b (the kernel A/B) is **dropped**: kernel monitor injection through
`AF_PACKET` measures the socket queue, not the radio (690k fps), so it would
have needed a witness-counted rebuild — and arm T answered the question
directly and more cheaply.

### Round 2 — Flash and an Opus subagent, 2026-09-20

Flash (9 findings) established that the gate never read `mt_async_stats`, so
submitted-vs-completed was invisible; that the ring is a hidden choice the
write-up never disclosed; and that arm V's verdict string ("the ACK is not
what the MAC is waiting for") claimed more than arm V could show. All folded
in: a `done/err` column per arm, a synchronous-path arm S, and softened
verdict language.

The Opus subagent overturned the conclusion outright. Its critical finding:
`gate_ucast` starts the MAC with `MT_RX_DRAIN_NONE`, and
`src/mt7612u/init.cpp:290-296` sets `ENABLE_RX` only when the caller will
drain EP 4 — so **the receiver was off for every arm**, and no arm could have
detected an ACK changing anything. It then did the arithmetic the write-up had
skipped (`MT_TX_RETRY_CFG` 15 retries, `MT_WMM_CWMIN/CWMAX` 15/1023, 9 µs
slot → ~46 ms per frame against 45.5 ms measured), showed the `busy%`
corroboration was sign-inverted, and specified the two missing arms. Both were
implemented (R and T) and arm T reversed the verdict.

Both reviewers' critical findings were verified against the source by hand
before being accepted. Nothing was rejected.

## Phase 1 — shared frame and crypto layer. No new behaviour.

Promote the software CCMP primitive (`tests/ccmp_software.h`, currently local
and uncommitted on `bench/mt7612u-ccmp`) into the tree with RFC-3610 and
802.11 CCMP known-answer tests in ctest — PR #335's review named missing KATs
as the single highest-leverage gap in its 6000 lines. Extract the management
frame builders and parsers the AP harnesses open-code into a shared module and
switch `ap_responder.cpp` and `ap_wpa2.cpp` onto it.

**Acceptance.** KAT cells green in `ctest`; `tests/mt7612u_ap_onair.sh` still
reads 14/14 on ch36 and 14/14 on ch6 (the merged baseline in
`docs/mt7612u-ap-mode.md`). Behaviour change of exactly zero.

**Status:** not started. Independently useful — it improves the AP side whether
or not a station is ever built.

## Phase 2 — the backend seam

`SetStationIdentity` / `ClearStationIdentity` on `IRadio`, the MT7612U
implementation, and the `AdapterCaps` flag. The risk is register ownership:
`MT_MAC_ADDR` already has two co-owners (the beacon and the ACK responder) with
explicit hand-off on both paths, and station arming is a third writer — the one
that must *not* move it (`docs/station-mode-scope.md` R6).

**Acceptance.** A headless selftest of the ownership hand-off in the style of
`tests/ack_responder_selftest.cpp`; an on-air regcheck that arms, reads the APC
slot back (R5 — a wrong slot is silent), disarms, and proves the registers
returned to their pre-arm values.

**Status:** not started. Blocked on Phase 0.

## Phase 3 — pure station logic, offline

The BSS table, the association state machine, the EAPOL/4-way supplicant, over
a crypto interface so `libdevourer` gains no dependency. No hardware.

**Acceptance.** `ctest` cells including two explicit negative cases, both of
them PR #335 defects that shipped in a reviewed PR: **a forged EAPOL-Key MIC
must be rejected**, and **an equal-counter replayed group rekey must be
rejected**. A phase that cannot fail those two tests has not been tested.

**Status:** not started.

## Phase 4 — the harness

`tests/sta_client.cpp` and `tests/mt7612u_sta_onair.sh`, cells graded pass/fail
in the style of `mt7612u_ap_onair.sh`: `open`, `wpa2`, `reconnect`, `bench`.

**Acceptance.** The harness contains no backend branch — see "the end goal"
above; this is the structural test, and it is checkable by reading.

**Status:** not started.

## Phase 5 — validation, independent witness first

Against **hostapd on non-MediaTek silicon** before anything else.
`docs/mt7612u-ap-mode.md` states its own weakness plainly — its station was the
same silicon, so it was not an independent-generation witness. The station work
should not repeat that. Then devourer-to-devourer against `ap_wpa2.cpp` on a
second adapter, which is the FPV shape. Then throughput and latency, each
number with its adversarial counterpart in the same breath.

**Status:** not started.

## Phase 6 — the Realtek arm, deferred to its own issue

`SetStationIdentity` on Jaguar1/2/3, with PR #335's `src/apfpv/StationMode.cpp`
as the reference (the kernel `hw_var_set_opmode` STATION path) plus its
`SetStationRxFilter`. Its close-range DIG-saturation finding is worth landing
independently of the rest.

This phase is the proof of the structural claim: if it is only an `IRadio`
implementation and the harness is untouched, the seam was right.

**Status:** not started. Deliberately after a working MT7612U station, not
alongside it.
