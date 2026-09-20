# Station mode — the plan, its gates, and the review ledger

The technical scope is `docs/station-mode-scope.md`. This file is the **living
plan**: what the phases are, what each gate demands, and which adversarial
reviews each gate has actually survived. It is updated as work lands, not
written once.

## The end goal, stated so a gate can be judged against it

A **harness** that fits devourer's existing structure and lets devourer join a
real access point — scan, authenticate, associate, WPA2-PSK 4-way as the
supplicant, encrypted data plane.

**Widened 2026-09-20.** The target decision in `docs/station-mode-scope.md`
adds the other half: the AP side must serve an ordinary BSS — multiple
associated stations, with the AP relaying between them — for clients we
configure (power save off; serving a genuinely unmodified client needs DTIM
buffering, which stays out of scope). That work is Phase 2b below. The MT7612U is the guinea pig: the first and
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
| Phase 1 | `src/sta/*` + harness switch | Flash (Dot11 vs the standard) | changes required | 9 | yes |
| Phase 1 | `src/sta/*` + harness switch | Flash (byte-equivalence) | claim upheld | 5 | yes |
| Phase 1 | `src/sta/*` + harness switch | Opus subagent | **API defect found** | 13 | yes |
| Phase 1 gate | `tests/mt7612u_ap_onair.sh` @ e26d12c | Flash (harness logic) | changes required | 8 (F1–F8) | yes |
| Phase 1 gate | `tests/mt7612u_ap_onair.sh` @ e26d12c | Flash (evidence vs claims) | **overclaims found** | 10 (F1–F10) | yes |
| Phase 1 gate | `tests/mt7612u_ap_onair.sh` @ e26d12c | Opus subagent | **two checks could not fail** | 14 + clean bills | yes |

### Round 5 — the Phase 1 gate, 2026-09-20

The gate's own acceptance harness, reviewed after it first read 14/14. All
three reviewers independently reached the same top finding: **the score was
real in that the checks ran and passed, and two of those checks could not
have returned anything else.**

What it caught, and none of this was in `src/sta/`:

1. The auto-ACK witness proved the AP's *receiver* worked, not its ACK
   responder — and the previous round had widened it after the evidence
   disagreed, while describing that as "not a weaker bar". Replaced with the
   station's own `tx_retries`/`tx_failed`.
2. Every "beacon is gone" check passed on a scan that returned nothing, which
   is also what a scan that never ran returns. That is the only air evidence
   this suite has that `StopBeacon` works.
3. The BCN_TU=25 rationale was contradicted by the harness itself — the `stop`
   cell hardcodes 100 TU and its scan checks always passed.
4. Three mysteries were retired on one ordered pair of runs, one of whose
   symptoms (the 524 ms RTT) neither arm reproduced.
5. A stale `STA_SYSFS` would re-enumerate whatever was plugged there, with
   none of the VID:PID guarding the AP's identical toggle already documented.

Both verdict functions now have a headless ctest guard
(`tests/ap_onair_witness_selftest.sh`) with every case verified able to fail —
the reviewers noted that nothing in CI protected any of this.

The reviewers also issued explicit clean bills worth keeping: the awk's
block-state machine and its BSSID/SSID/frequency keying were verified correct
against fixtures; `iw connect`'s argument order is right and pinning
frequency+BSSID is strictly *stricter*; `came_up()` greps a string the MT7612U
backend emits only after a successful arm; and the `env` fix to the deleted
RTL script was the correct diagnosis of a real shell trap.

### Round 3 — Phase 1, 2026-09-20

Three reviewers, one checkpoint. The byte-equivalence reviewer traced every
emitted byte and **upheld** the refactor's claim: AAD, nonce, CCMP header,
ciphertext placement and both IE encodings identical, and the QoS erase exact
with no off-by-two. The other two found real defects.

The one that mattered: `ccmp_encrypt` copied 24 header bytes and
`ccmp_decrypt` hardcoded 24, while both accepted a `qos_tid` argument — so a
real 26-byte QoS header would have had its QoS Control octets silently
overwritten by the CCMP header. **A station's data plane is QoS against any
802.11n AP, so this would have broken Phase 2 with no diagnostic.** Both now
take an explicit header length and derive the TID from the frame.

The one that was most uncomfortable: the known-answer vectors claimed to be an
*independent* transcription of the standard. They were not — same author, same
reading, matching idioms — and the consequence was concrete: three of four
vectors encoded frame shapes that were non-conformant or impossible on air,
with generator and implementation agreeing. One vector's comment claimed it
carried every masked bit while Retry was clear in it. All rebuilt as real
frames; the generator now states plainly that it pins the cipher plumbing and
not the framing rules.

Nothing was rejected. Every critical finding was verified against the source
by hand before being accepted.

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

**Status: GATE CLOSED.** See “Closing the gate” at the end of this phase.

Done:
- The `bench/mt7612u-ccmp` artifacts are landed, rebased onto master. That
  branch turned out to be a label six commits behind master with **no commits
  of its own** - every artifact Phase 1 depends on was uncommitted working-tree
  state, one `git checkout` from gone.
- `src/sta/CryptoOps.h` - the crypto interface, so `libdevourer` gains no
  dependency and the protocol logic is testable without a crypto library or a
  radio.
- `src/sta/Ccmp.h` - the 802.11 CCMP framing (AAD, nonce, header, PN,
  encrypt/decrypt, replay window) promoted out of `tests/ap_wpa2.cpp`, written
  over `CryptoOps` rather than over OpenSSL.
- `ctest ccmp_framing` - the KAT cell, against vectors generated by
  `tests/ccmp_gen_vectors.py` from python-cryptography's AESCCM with the 802.11
  framing transcribed independently of the header under test. Plus the AAD
  masking rules, the 48-bit PN packing, MIC and ciphertext tamper rejection,
  and the replay gate's `<=`. **The cell was verified to fail**: injecting the
  classic AAD slip (forgetting to mask Retry) turns it red, reverting turns it
  green. 70/70 ctest green.

Also done since:
- `src/sta/Dot11.h` — the management-frame module, and **both AP harnesses
  switched onto it**. That switch is the part that proves the module is
  role-neutral rather than a station module with AP-shaped holes.
- Three adversarial reviews (round 3 above), all findings resolved.
- On-air, against an **RTL8812AU station on the in-tree rtw88 driver (module `rtw88_8812au`, sysfs driver name `rtw_8812au`)** — independent
  silicon, which is a stronger witness than the same-MediaTek station
  `docs/mt7612u-ap-mode.md` used and states as its own weakness:
  - beacon aired and **scannable** (`SSID: devourerAP`, `BSS 02:42:75:05:d6:00`);
  - **open auth + assoc complete** — `connected to 02:42:75:05:d6:00`, with
    the AP logging `AUTH req` and `ASSOC req`;
  - **WPA2 4-way complete** — `WPA: Key negotiation completed … [PTK=CCMP
    GTK=CCMP]`, `CTRL-EVENT-CONNECTED`, AP side `msg2 OK (MIC verified) →
    msg3 → msg4 OK`.

**The encrypted data plane did not carry on this rig, and it is not the
refactor.** The station disconnects on inactivity (reason 4) after a completed
handshake, and ping is 100% lost. The control settles it: the **pre-refactor**
`ap_wpa2`, built from `1139782` and run against the same station in the same
session, fails identically — same completed 4-way, same reason-4 disconnect,
same 100% loss. So the failure predates Phase 1 and belongs to this rig's
station, which has never been used with these harnesses (the validated runs
used an MT7612U kernel station). Without that control the run would have read
as a refactor regression.

### The four items flagged before Phase 3 — all closed

**1. The encrypted data plane is demonstrated.** MT7612U AP, RTL8812AU station
on the in-tree rtw88 driver (module `rtw88_8812au`, sysfs driver name `rtw_8812au`), 2.4 GHz:

```
  data plane: encrypted frames received=98, MIC failures=0,
              replays rejected=1, frames sent=27
  5 packets transmitted, 5 received, 0% packet loss
```

Zero MIC failures across ~250 encrypted frames over two successful runs, so
the promoted CCMP module decrypts a real station's traffic correctly.

**Correction, from review:** an earlier revision of this section said the
window "rejected one genuine on-air replay". That is not supported — a
retransmitted MPDU carries the same PN as the original, so a strict counter
cannot tell an attack from a MAC retransmission, and a retransmission is by
far the more probable explanation for a single rejection. What the `1` shows
is that the window is *reached and exercised* on air, which is still the point
worth making: it is deployed rather than a test fixture.

**The earlier 100% loss was ours, not the rig's.** The leading hypothesis in
the previous revision — that the Realtek station sends QoS data the
pre-refactor AP could not decrypt — was **wrong**: the QoS AAD correction
shipped a commit before that run and the run still lost every packet. By
elimination the cause was the data-plane sequence number, item 2 below. That
is inference from an ordered pair of runs rather than an A/B, and it is stated
as inference.

**2. The data planes air sequence numbers.** Both harnesses' data headers now
come from the shared builder with a real 12-bit counter. This was the fix that
turned the data plane on, and it is exactly the mechanism review round 1
predicted when it found the sequence-number gap: *"a station's data plane
feeds the AP's duplicate detector, where a pinned seq=0 is precisely what gets
dropped."* The prediction was right and the first on-air run mistook its
consequence for a rig fault.

**3. `CcmpReplay` is called.** Checked after the MIC verifies and never
before, keyed by the frame's TID, reset on every PTK install.

**4. RSN parsing no longer rejects real APs.** `parse_rsn()` reads the counts
and searches the suite lists instead of byte-comparing a canonical
one-of-each layout, so mixed-mode WPA/WPA2 and WPA3-transition BSSes are
joinable. MFP-required BSSes are refused, and surfaced as such rather than
failing the handshake later. The test was verified to catch the old
semantics.

Also closed on the way: `SeqCounter` is atomic; `build_probe_req` no longer
emits a 2.4 GHz-only DS Parameter Set on 5 GHz; the station builders abort on
an over-length SSID instead of airing a frame missing it; and both harnesses
now derive their header length from `data_hdr_len()` rather than
`fc0 == 0x88` — I had fixed that exact-equality test in the shared module and
left it in the harnesses, where it would have disagreed with the `is_qos_data`
mask used two lines below it.

## Closing the gate

`tests/mt7612u_ap_onair.sh` now reads **14/14 twice on 2.4 GHz (ch6) and twice
on 5 GHz (ch36)**, at the **default** `BCN_TU=100`, against an RTL8812AU on
the in-tree rtw88 driver — independent silicon on both ends, which is what
`docs/mt7612u-ap-mode.md` admits its own same-silicon witness lacked.

Getting there took two review rounds and cost four defects in the harness plus
one in the WPA2 authenticator. None was in `src/sta/`, which is the Phase 1
deliverable — worth saying, because it is the only part of this that Phase 2
builds on.

### What the first run found: station power save

Neither AP harness implements 802.11 power save. Verified by reading all three
beacon builders at the time: **none of them appended a TIM element** (two
now do — see the note below), though
`src/sta/Dot11.h` defines `kEidTim`. A beacon with no TIM is not a conforming
AP beacon (802.11-2016 9.4.2.6) and gives a dozing station no DTIM schedule;
nothing is buffered either. Open network, 60 pings at 1/s:

**Superseded:** the AP harness beacons now carry a TIM (`append_tim`, `src/sta/Dot11.h`); `tests/mt7612u_beacon_stop_check.cpp` still does not. Buffering is still absent, so power save is still unsupported — that half stands.

| | received | RTT |
|---|---|---|
| `power_save on` | 0/60, link dropped mid-run, AP saw 2 of ~62 | — |
| `power_save off` | **60/60, 0% loss** | 0.735 / 1.530 / 7.644 ms |

**That is one ordered pair, not an A/B**, and it is not enough to retire the
three items this document previously listed as unexplained. Specifically: the
524 ms / 1729 ms RTT was measured on the WPA2 path and is reproduced by
*neither* arm here — the PS-on arm received nothing at all, so it has no RTT —
and "roughly half the runs carry no packet" is a population claim that a
0/60-vs-60/60 pair cannot explain by itself. Related, and large enough to act
on. Explained, not yet. The three items stay open below, qualified.

**The gate's scope shrank as a result, and that has to be said in the same
breath as the 14/14:** a score obtained with power save forced off does not
certify this AP against a default-configured Linux station, because Linux
defaults to `power_save on`. This is now in `docs/ap-mode.md`'s out-of-scope
list, where it never was.

### What the second round found: two checks that could not fail

- **The auto-ACK witness was decoration.** It grepped the AP's log for a
  management frame at retry=0; the AP logs every received copy and reception
  does not depend on ACKing, so a disarmed ACK responder passed it. And the
  previous round had *widened* it from AUTH to AUTH-or-ASSOC after AUTH came
  in at retry=1 in four runs of five — widening a predicate until the evidence
  stops disagreeing, which that round then described as "not a weaker bar".
  Replaced by the station's own `tx_retries` / `tx_failed`, which is the only
  side that knows whether its frames were acknowledged. Measured: 25 frames,
  0 retries, 0 failed.
- **"The beacon is gone" passed on a dead scan.** Absence checks pass on zero,
  so a scan that errored or never ran read exactly like a silenced beacon —
  and that is the only air evidence this suite has that `StopBeacon` works.
  It now requires the same scan to have seen some BSS. The control caught a
  real event on its first run.

Both are now guarded headlessly by `tests/ap_onair_witness_selftest.sh` in
ctest, 14 cases each verified able to fail. Nothing in CI protected them
before.

- **The authenticator never retransmitted.** `ap_wpa2` sent msg1 and msg3 once
  each, so a single lost frame stalled the handshake permanently — which is
  what the wpa2 cell failed on. Fixed per 802.11-2016 12.7.6.4, reusing the
  ANonce and GTK and incrementing the Key Replay Counter.

### What is still not done, stated plainly

- **`BCN_TU=25` was never needed, and this harness was always the evidence.**
  The `stop` cell's binary hardcodes 100 TU and ignores the variable, and its
  scan checks passed in every run. `docs/ap-mode.md`'s "a dense beacon
  interval is needed throughout" does not reproduce on this bench; the earlier
  claim here that this station "misses a 100 TU beacon entirely" is withdrawn.
- **The AUTH-at-retry=1 asymmetry is unexplained.** Five runs, AUTH at retry=1
  in four; ASSOC at retry=0 in all five. The station's counters are created
  with the peer entry at association, so the new witness says nothing about
  the exchange that precedes it. Not closed.
- **The link is unstable on this rig** and **RTT was poor** (524 ms mean,
  1729 ms max) — both now *probably* power save, on the evidence above, but
  neither is demonstrated. Post-fix RTT is 0.7–2.4 ms across every cell.
- **The 5 GHz open cell's data-plane check is brittle.** It failed once in
  three runs on a single lost ping: 6 packets at 0% loss, on a channel with
  several strong neighbours. Not loosened — recorded.
- **Nothing here is soaked.** Longest run about two minutes; n=2 per band,
  one AP unit, one station unit, near field.
- **A second cfg80211 cache entry for our own BSSID** appears while the AP is
  up, carrying a frequency we are not airing on. Its origin is not
  established. The harness now pins frequency and BSSID on connect, which
  makes the cell deterministic **without** explaining the entry.

**A caveat that belongs in the record:** the vectors are cross-implementation,
not official. The IEEE 802.11-2016 Annex J CCMP vector would be strictly better
and is a drop-in replacement; `tests/ccmp_gen_vectors.py`'s header says so
rather than letting a reader assume these are the standard's own numbers.

## Phase 2 — the backend seam

`SetStationIdentity` / `ClearStationIdentity` on `IRadio`, the MT7612U
implementation, and the `AdapterCaps` flag. The risk is register ownership:
`MT_MAC_ADDR` already has two co-owners (the beacon and the ACK responder) with
explicit hand-off on both paths, and station arming is a third writer — the one
that must *not* move it (`docs/station-mode-scope.md` R6).

**Acceptance, as originally written.** A headless selftest of the ownership
hand-off in the style of `tests/ack_responder_selftest.cpp`; an on-air
regcheck that arms, reads the APC slot back (R5 — a wrong slot is silent),
disarms, and proves the registers returned to their pre-arm values.

**Status: implemented, gate NOT closed. The acceptance criterion above is
half unmet, and the measurement it assumed has been inverted.**

Landed: the `IRadio` seam with its contract, `AdapterCaps::station_mode_ok`
(**true** for MT7612U), the MT7612U implementation, five bring-up gates
(`sta`, `staack`, `staid`, `norsp`, `bssen`), a headless selftest
(`tests/mt7612u_station_selftest.cpp`), and three harnesses
(`mt7612u_sta_identity.sh`, `_autoack.sh`, `_uplink.sh`).

What R5 and R6 actually returned — `docs/mt7612u-station-identity.md`, and
read its retraction section first:

- **R5 answered.** The BSSID registers do not gate a managed station's
  receive: 5877 unicast frames with both registers deliberately wrong against
  6250 with nothing programmed. So `SetStationIdentity` writes neither.
- **R6 ANSWERED** by a third method, after two failed: ask the transmitter.
  A Realtek peer injects unicast at the DUT and reads its own CCX reports —
  100% acknowledged at 0.45 mean retries with nothing armed, against three
  controls pinned at the retry limit. The 0.8%/98% figures from the second
  method are retracted. The table was re-taken 2026-09-20 under a corrected
  single-variable harness — the first one's claim arm ran the monitor filter —
  and the conclusion is unchanged.
- **The prohibition on moving `MT_MAC_ADDR` stands on better evidence than
  it was written with.** Under the managed filter it takes reception from 103
  frames to zero: the port identity gates what a station *receives*.

Why the first run of these measurements was worthless, because it is the
lesson rather than the result: the gate called `mt7612u_set_monitor_rx()`
under a comment reading "managed filter, not monitor". It is the opposite —
that call clears every address and BSS drop bit — so six identical arms were
guaranteed before a frame arrived. Phase 0's defect, in a gate whose own
comment congratulated it for not repeating Phase 0. Round 1's "correction" to
R5 (bit 3 is clear, so the filter is not in play) is what made it invisible;
bit **2** is the one that matters and it is set.

Still open against the original acceptance text:

- ~~No headless selftest of the ownership hand-off.~~ Added in `55f23da`:
  `tests/mt7612u_station_selftest.cpp`, `ctest` now 73. The policy was split
  out of `station.cpp` into `StationIdentity.h` so it could be tested at all.
- **No register round-trip proof**, because the implementation writes no
  registers — the criterion assumed an arm that programs the APC slot, and
  the measurement says it should not. The criterion is stale, not merely
  unmet, but it has not been rewritten and nothing yet reads the APC slot
  back, which R5's own text asked for.
- `station_mode_ok` is now **true** for MT7612U: both halves of its bar are
  measured — see `docs/mt7612u-station-identity.md` for the arms, the controls
  and the four limits that travel with it.

## Phase 2b — the AP-side BSS. Added 2026-09-20 by the target decision.

The scoping decision in `docs/station-mode-scope.md` ("The target: an ordinary
BSS, with the AP bridging") named eight work items and, in its first form,
recorded them **only there** — no gate, no acceptance evidence, no ledger row,
for eight steps of which three are security-relevant. That is a direct miss of
rule 4 above, caught by the Opus continuity review, and this section is the
correction.

**Why it is 2b and not part of Phase 3.** Phase 3 is *station* logic, offline.
Every item here is *AP-harness* work on hardware-facing code
(`tests/ap_wpa2.cpp`, `tests/ap_responder.cpp`, `tests/ul_trigger_ap.cpp`,
`src/sta/`). The two sets do not intersect, and the decision document's order
of work is not a substitute for a phase.

**Ordering against Phase 3.** 2b.1 (the CCMP nonce) is **not optional and not
deferrable** — it is a live defect in merged code, it is what Phase 5 would
otherwise discover against a real client, and it must land before any per-TID
PN counter exists anywhere. The rest of 2b may run in parallel with Phase 3 or
after it; nothing in Phase 3 depends on it.

| # | Item | Gate |
|---|---|---|
| 2b.1 | Fix the CCMP nonce flags octet; re-source vectors from IEEE Annex J | A ctest cell that FAILS against the current `nonce[0] = 0` and passes after. Vectors must not come from `ccmp_gen_vectors.py` — that file shares the misreading |
| 2b.2 | Per-station table in `src/sta/` (assoc state, AID, PTK, TX PN, per-station `CcmpReplay`) | ctest, pure and backend-agnostic; a two-station fixture the old single-`g_sta` code cannot satisfy |
| 2b.3 | Pass the real SA; read addr3 | ctest on the frame builders; a relayed header byte-compared against Table 9-26 |
| 2b.4 | Real DHCP address pool + binding table | Two stations lease two distinct addresses on air |
| 2b.5 | Association-table ARP responder answering with the target's real MAC | A resolves B and gets **B's** MAC, not the AP's |
| 2b.6 | GTK transmit path: key id 1, one GTK per BSS, own PN space | A second association must not revoke the first station's group key. On-air: a group frame decrypts at both stations |
| 2b.7 | Intra-BSS relay, with duplicate detection and a stated position on fragmentation and A-MSDU | A pings B through the AP. Negative control: a retransmitted MPDU is relayed **once** |
| 2b.8 | TAP forwarder in `tests/` or `tools/`; 802.11 ↔ 802.3 helper (incl. the Ethernet II header) in `src/sta/` | Host stack reaches a station through the TAP; the doc states which side owns ARP/ICMP |

**Acceptance for the phase as a whole.** Two associated stations, both
configured with power save off, exchanging encrypted unicast through the AP,
plus a group-addressed frame that both decrypt. And per rule 2, two adversarial
reviews with every finding resolved.

**Structural property to hold, stated because Phase 1's equivalent is what made
that phase checkable:** none of 2b.1-2b.7 may add a backend branch. All of it
is `src/sta/` or harness code. Only 2b.8 is allowed to be OS-specific, and that
is why it lives outside the library.

**Status:** not started. Decision recorded 2026-09-20; reviewed by two Flash
reviewers and one Opus continuity review the same day, whose findings are in
the scope document's own correction table and in the ledger below.

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
