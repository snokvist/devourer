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

### Review cadence (set 2026-09-20)

Two tiers, because they catch different things and cost different amounts:

- **Every gate, including a sub-item like 2b.3: one adversarial Flash
  reviewer**, pointed at the specific diff and asked for *concrete defects with
  a failure scenario*, not design opinions. This is the surface-bug pass — use-
  after-unlock, a lock taken twice on one path, an off-by-one, a caller that
  ignores a new return value. Cheap enough to run on every item, and the
  failures it catches are the ones that survive a clean ctest.
- **End of a larger gate (a whole phase, or a decision document): an Opus-led
  review batch.** Several reviewers with *different angles* — protocol
  correctness, architecture and internal consistency, and continuity with what
  the repo already decided. The continuity angle is the one that is easy to
  forget and has found the most: a correct decision recorded in a document
  whose neighbours still say the opposite.

Prompt both tiers to state explicitly when a category yields nothing, rather
than padding — a reviewer that always finds something is as uninformative as
one that never does.

### Performance measurement in a gate

Throughput does **not** belong on every gate. Near-field RF variance on this
bench swamps the regressions worth catching, so a per-gate throughput number
would either miss a real one or cry wolf, and a gauge that cannot tell a
regression from its own noise gets ignored. What belongs on a gate is the
**deterministic** part:

- the per-frame CCMP cost the AP already emits under `DEVOURER_CCMP_PROFILE`
  (`ccmp_tx_ns_per_frame` / `ccmp_rx_ns_per_frame`), which is CPU work per
  frame and barely moves with RF conditions;
- the headless `ccmp_sw_bench`, which has no radio in it at all.

The on-air `bench` cell of `tests/mt7612u_ap_onair.sh` stays what it is: an
occasional measurement run deliberately, not a gate. And note what it is not —
a flood ping is round-trip bound, so `reply_pps` is a *latency* figure, not
link throughput. Nothing in this tree measures throughput yet.

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
| 2b.1 nonce interop | `b716aa2` | Flash (adversarial) | changes required | 4 (F1–F4) | yes — `7dd6f91` |
| 2b ctest target | `7dd6f91`+`8a3ccd9` | Flash (adversarial) | changes required | 4 (F1–F4) | yes — `903c69e` |
| Phase 3 | `264a6f7` | Flash (protocol + memory safety) | **state-machine defect** | 5 | yes — `03d2478` |
| Phase 3 | `264a6f7` | Opus subagent (continuity) | **rule 4 violated again** | 12 (F1–F12) | yes — `03d2478`, `86d5ad5`, this commit |
| Phase 3 | `264a6f7` | Opus subagent (architecture) | **replay counter poisonable** | 11 | yes — `03d2478` |
| Phase 3 | `0dc7d70` | Flash (hostile input to every parser) | **AAD defect** | 3 | yes — `2fef219` |
| Phase 3 | `0dc7d70` | Flash (can any assertion fail?) | **6 unfalsifiable cells** | 7 | yes — `2fef219` |
| Phase 3 | `0dc7d70` | Flash (branch-wide integration) | changes required | 11 | yes — `2fef219` |

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
| 2b.1 | ~~Fix the CCMP nonce flags octet~~ **DONE 2026-09-20, interop CLOSED 2026-09-21** | Gate met: mutation reintroducing `nonce[0] = 0` fails 8 checks (4 vector cells + 4 direct assertions in `test_nonce_flags()`); regeneration changed only the two QoS vectors. The interop half, originally "not met", is now closed by `tests/ccmp_kernel_vectors.h` — sixteen QoS frames the Linux kernel encrypted, all eight TIDs, captured off a virtual `mac80211_hwsim` rig; the same mutation breaks 14 of the 16 and leaves the two TID-0 ones green. An on-air TID 1..7 cell still rides with the WMM work |
| 2b.2 | ~~Per-station table in `src/sta/`~~ **DONE 2026-09-20** — `src/sta/StationTable.h`, `tests/station_table_selftest.cpp`, ctest 74 | Gate met: `test_two_stations_are_independent()` holds two PTKs, two TX PN spaces and two CCMP windows, and a mutation collapsing `add()` to slot 0 is caught. 4 mutations run, 3 caught; the survivor (deleting `add()`'s redundant wipe) is recorded in the test rather than hidden. **Wired and device-verified 2026-09-20**: `tests/ap_wpa2.cpp` runs on the table, and TWO STATIONS ASSOCIATED AT ONCE — see "The two-station cell" below. `ap_responder.cpp` and `ul_trigger_ap.cpp` still use their own `g_sta` |
| 2b.3 | ~~Pass the real SA; read addr3~~ **DONE** | `data_da`/`data_sa`/`data_da_is_group` in `Dot11.h`; relayed header byte-compared against Table 9-26 by hand, not against the builder. The cell caught its own author — the first version passed `data_hdr_to_ds`'s arguments in the wrong order and failed eight checks |
| 2b.4 | ~~Real DHCP address pool + binding table~~ **DONE** | The station table IS the binding table: address = `192.168.99.(1 + aid)`, so nothing can outlive its lease or be double-allocated. On air: `DHCP: ACK 192.168.99.2 to aid=1` and `192.168.99.3 to aid=2` |
| 2b.5 | ~~Association-table ARP responder~~ **DONE** | `Unicast reply from 192.168.99.3 [40:A5:EF:2F:22:9B]` — B's own MAC, not the AP's, which is what keeps the traffic at layer 2 instead of making the AP an L3 hop |
| 2b.6 | ~~GTK transmit path~~ **DONE** | Key id 1 (the TX path hardcoded 0, so every group frame was looked up as the pairwise key and MIC-failed), one GTK per BSS, its own PN space. Probed with an ARP for an address **nobody holds**, so the AP's responder cannot answer and the flood is the only path: B received it decrypted |
| 2b.7 | ~~Intra-BSS relay~~ **DONE** | A pings B through the AP: 100% loss → **4/4, 0% loss**, `relayed=8 dropped=0`. Duplicate detection turned out to exist by construction (an 802.11 retransmission carries the same PN, and `CcmpReplay::accept` gates the relay decision) — the earlier "no implementation" note was wrong. Fragmentation and A-MSDU are now **refused and counted**, which is the stated position the gate asked for |
| 2b.8 | ~~TAP forwarder~~ **DONE 2026-09-21** | `sta::msdu_to_eth`/`eth_to_msdu` in `src/sta/Dot11.h` (6/6 mutations caught); the forwarder is `DEVOURER_AP_TAP=<ifname>` in `ap_wpa2.cpp`, off by default. Gate met with the station's PHY in its own netns so the TAP is the only route: **4/4, 0% loss**. **Who owns ARP/ICMP/DHCP: the HOST**, and the userspace responders are disabled whenever a TAP is open |

**Acceptance for the phase as a whole.** Two associated stations, both
configured with power save off, exchanging encrypted unicast through the AP,
plus a group-addressed frame that both decrypt. And per rule 2, two adversarial
reviews with every finding resolved.

### Phase 2b — COMPLETE 2026-09-21

All eight items done. The forwarding decision was lifted out of the harness
into `sta::decide_forward()` (6/6 mutations caught) so that four of those gates
stop resting on narrated bench runs, and so 2b.8 could reuse the same decision
rather than writing a second copy.

**Two gates nearly passed without meaning anything, and both are worth
remembering.** The TAP cell's first run read 4/4 with 0% loss while the AP's
ledger showed only one non-group frame arriving over the air — both addresses
were local to one host and the kernel had routed between them internally. Its
second run read 100% loss because the root namespace reached the station
through the 8812CU, which still held an address from the relay cell. The cell
now clears competing addresses and **asserts the route before measuring**,
aborting rather than reporting a number about a different interface.

**Still open, carried out of the phase rather than closed inside it:**

- ~~`tests/ap_wpa2.cpp` has no ctest target of its own.~~ **CLOSED
  2026-09-21**, see below.
- The AID→IP derivation couples identity to a reusable slot index, so an
  unauthenticated deauth costs more here than on a normal AP, where a lease is
  keyed on MAC. Inherent to the "table IS the binding table" simplification.
- ~~2b.1's nonce fix is still interop-unproven.~~ **CLOSED 2026-09-21**, see
  below. An on-air TID 1..7 cell still rides with the WMM work, but it is no
  longer the only thing that could close this.
- Two stations, not seven, and no churn during traffic.

### The two carried-forward items, closed 2026-09-21

Both were closed before starting Phase 3, on the reasoning that a phase should
not inherit a hole that a later phase is expected to repeat.

**The nonce interop gap.** Closed with vectors the LINUX KERNEL produced, not
with a second reading of the standard. `tests/ccmp_capture_vectors.sh` builds
a two-radio `mac80211_hwsim` rig — no hardware, so the shared bench is
untouched — runs hostapd with `wmm_enabled=1` and wpa_supplicant over it,
sends one datagram per user priority in each direction, and cuts sixteen
protected QoS data frames into `tests/ccmp_kernel_vectors.h`. All eight TIDs,
both directions. The MIC is the oracle: CCM authenticates the AAD and the
nonce, so one wrong bit in either fails the tag. Each vector is proved twice —
the kernel's MIC must verify under our framing, and re-encrypting the
recovered plaintext must reproduce the captured MPDU byte for byte.

`ping -Q` does not give TID control (setting IP_TOS sets `sk_priority` through
`ip_tos2prio[]` before `cfg80211_classify8021d` ever looks at the DSCP), which
is why `tests/ccmp_tid_send.py` sets `SO_PRIORITY` to 256+tid instead. The
first attempt produced TIDs 0,1,4,5,7 and was nearly accepted as coverage.

Nine mutations of `src/sta/Ccmp.h`, one at a time. The original defect —
`nonce[0] = 0` — now fails 15 kernel-vector assertions and leaves the two
TID-0 vectors green, which is the shape of its survival. Four more are caught
(PN endianness, the QoS subtype mask, the CCMP header's reserved byte and its
Ext IV bit). Three are **not** caught by these vectors and stay covered by the
direct assertion cells: the nonce's Management bit (the capture has no
protected management frames — no PMF), the AAD's fragment-number masking (no
fragments), and its retry masking (no retransmissions). The ninth — dropping
the `& 0x0f` that keeps only the TID out of the QoS Control field — **survived
the entire file, kernel frames included**, because every frame anywhere in
this tree carries a plain TID with a zero upper nibble. `test_qos_aad` now
builds its own header with EOSP, ack policy 3 and A-MSDU-present set. A real
WMM/HT station sets those bits constantly.

**The missing ctest target.** `ap_wpa2 --self-test` is ctest cell 75. It
returns before libusb is opened: no adapter, no root, no airtime, no
`/dev/net/tun`. Twelve cells in `tests/ap_wpa2_selftest.inc` drive the real
`on_rx()` with synthetic frames and read the real transmit queue back, playing
the supplicant end to end — the two-station handshake, a forged msg2, the key
replay counter's window, a duplicate msg2, the relay, a data replay, a MIC
failure, a QoS uplink, the group flood, both refusals, the ARP responder, and
the TAP in both directions.

It found a defect the day it was written. `decide_forward()` refused a
fragment by testing the **More Fragments bit alone** — and that bit is CLEAR
on the LAST fragment of a fragmented MSDU. The tail of another MSDU, carrying
no LLC/SNAP header, was forwarded to a peer as a whole frame while every
counter read success. The comment claiming this "cannot reach this AP while it
advertises neither WMM nor HT" was wrong twice: fragmentation has no
relationship to either, and any legacy station with a fragmentation threshold
set produces it. Both halves are checked now.

Thirteen mutations of the AP, one at a time; twelve caught. The one survivor
is a wrong CCM nonce, and it is recorded in the cell rather than hidden: this
file is a round trip, both ends call the same `ccmp_encrypt`/`ccmp_decrypt`,
so a wrong TID is wrong identically in both directions and the MIC still
verifies — which is precisely how the 2b.1 defect survived. The kernel vectors
are what catch it. One earlier survivor was fixed rather than recorded:
pinning the relay's PN to a constant passed every assertion in the file, and
two frames under one key at one PN is keystream reuse. A cell now relays twice
and requires the second PN to be greater.

**Reviews.** Two adversarial Flash reviews, one per gate, eight findings
between them, all eight verified against the tree before acting and all eight
real. The first found that `ccmp_decrypt` had no output-capacity parameter at
all while `ccmp_encrypt` had always had one — a fixed-size buffer one full-MTU
frame away from a stack smash, in a function that processes frames from the
air and writes the plaintext out before the tag is checked. It is a required
argument now, with `ccmp_decrypted_len()` to size it and a negative arm that
refuses one byte too few. The second found the new ctest target guarded only
by `OpenSSL_FOUND` while `ap_wpa2.cpp` includes `<linux/if_tun.h>`
unconditionally (it would break the macOS build matrix), and named so that the
`selftests` aggregate the mingw job builds would not collect it — measured in
both arms rather than reasoned about.

**On air**, because three of the changes touch the AP's live path. MT7612U 1-1
as the AP, RTL8812AU 8-1 as the station: ch36 **14/14**, ch6 **14/14** on the
second run and **13/14** on the first. The 13/14 was one lost echo out of six
in the WPA2 cell; the ledger read `fragmented=0, A-MSDU=0, MIC failures=0,
replays rejected=0` — none of the counters the new checks would move — the
cell then ran three more times at 0% loss with an identical ledger, and the
bench cell measures this link at 0.4% loss over 3487 packets. Six packets is a
small sample in either direction; the ledger is the evidence, not the ping
count.

### Phase 2b review ledger

Rule 2 requires two adversarial reviews per phase with every finding resolved,
and the cadence set in this document requires one Flash reviewer per gate. The
per-gate tier was run on the 2b.1/2b.2 pair and at the boundary; it was **not**
run on 2b.3–2b.7 individually, which is a gap in the process rather than in the
code, and is recorded here rather than glossed.

| # | Reviewer | Angle | Outcome |
|---|---|---|---|
| R1 | Flash | Surface bugs in 3ea0d4e / a76bde4 / StationTable.h, weighted to concurrency | 4 substantive: the table never emptied, the EAPOL replay counter was unchecked, an HT-Control bound, a fresh ANonce on re-assoc. Two accepted, two rejected with reasons. Lock discipline came back clean |
| R2 | Flash | What one station can do to another | PTK committed before MIC verification (fixed); unauthenticated deauth frees a slot AND an identity, because the IP derives from a reusable AID; unauthenticated assoc fills the table. Four categories explicitly yielded nothing, including relay SA spoofing |
| R3 | Flash | Do the tests test what they claim | `ap_wpa2.cpp` is in no test target; the false independence claim still live in two code-facing places; a tautological AAD cell; two defects in cells written that day; IBSS row untested |
| R4 | Opus | Continuity at the boundary, and correctness of relay/GTK | 10 findings. Blockers: the table never emptied, the replay counter fought retransmission, the plan contradicted itself, fragmentation/A-MSDU silently corrupted. **Corrected my premise**: duplicate detection exists by construction. Crypto, locking, loop/amplification and group replay all explicitly clean |

**ff85aca shipped three behaviour changes with no row and no gate** — deauth
frees the slot, the EAPOL replay counter, the real AID in the association
response. One was security-relevant. That is the rule-4 miss this section
exists to correct, committed while correcting it.

**Resolved from the boundary review** in `873dff0`: the replay-counter window,
the PTK-before-MIC ordering, the give-up path freeing its record, the
fragmentation and A-MSDU refusal, the ledger arithmetic, and four test
defects. **Carried forward, not resolved at the time:** `ap_wpa2.cpp` had no ctest
target at all, so 2b.4–2b.7 rested on narrated bench runs (closed 2026-09-21 —
see "The two carried-forward items" above); the relay decision should
move into a pure function in `src/sta/` so it can be tested headlessly and so
2b.8 can reuse it rather than writing a second copy; and the AID→IP derivation
couples identity to a reusable slot index, which makes an unauthenticated
deauth cost more here than on a normal AP.

### Phase 2b acceptance — MET 2026-09-20, with 2b.8 still open

The bar was "two associated stations, both configured with power save off,
exchanging encrypted unicast through the AP, plus a group-addressed frame that
both decrypt". Both halves now hold, measured in one run:

```
A->B unicast   4 packets transmitted, 4 received, 0% packet loss
               rtt min/avg/max/mdev = 1.165/1.558/2.034/0.310 ms
B's interface  ARP, Request who-has 192.168.99.77 (ff:ff:ff:ff:ff:ff) tell 192.168.99.2
               IP 192.168.99.2 > 192.168.99.255: ICMP echo request
AP ledger      to this AP=328, to a peer station=8 (relayed=8 dropped=0),
               off-BSS=0, group frames aired=39
               encrypted frames received=336, MIC failures=0, replays rejected=0
```

The group probe is an ARP for **192.168.99.77, an address nobody holds**. The
AP's own responder cannot answer it, so the only way it reaches B is as a
flooded frame under the GTK — a probe the AP could answer would have proved
nothing.

**Two limits that travel with this result.** Both stations are interfaces on
one host in one subnet, so the cell lowers `rp_filter` and sets
`accept_local=1`; B was otherwise dropping perfectly good relayed frames as
martians, which cost a diagnostic pass to find. Separate hosts would need
neither. And this is two stations, not seven, with no churn during traffic.

**Still open at the phase boundary:** 2b.8 (the TAP forwarder), and within
2b.7 the duplicate detection, fragmentation and A-MSDU positions that its gate
asked for and that this implementation neither built nor refused in writing.

### The two-station cell — measured 2026-09-20

The first half of that acceptance bar is met. AP: MT7612U at `1-1` running
`tests/ap_wpa2.cpp` on ch6. Stations: RTL8812AU (`20:0d:b0:c4:a7:6a`) and
RTL8812CU (`40:a5:ef:2f:22:9b`), both on rtw88, both with power save off.

Sequence, run one command at a time rather than as a script, so each step's
result was visible before the next:

1. Station A associates, four-way completes, **5/5 pings** to the AP.
2. Station B associates, its own four-way completes.
3. **Station A pings again — 5/5.** This is the gate. Under the previous
   file-scope `g_ptk`/`g_sta`, B's association overwrote A's key material and
   A went dead; under the table it does not.
4. Both stations ping concurrently, 15 each: **0% loss on both.**

The AP's own exit summary, which is the table reporting on itself:

```
sent=82 stations=2 [aid=1 20:0d:b0:c4:a7:6a 4way_state=3]
                   [aid=2 40:a5:ef:2f:22:9b 4way_state=3]
data plane: encrypted frames received=36, MIC failures=0, replays rejected=0
```

Two records, two distinct AIDs, both at state 3 (Done). The two zero counters
are the discriminating evidence, not decoration:

- **MIC failures = 0** means no key cross-contamination: each station's frames
  decrypted under that station's own PTK.
- **replays rejected = 0** means the per-station receive windows did their job.
  A shared window is exactly where this breaks — B's ordinary PN 1..N are the
  same integers A already used, so a single window would reject all of them and
  the counter would be non-zero while B showed total loss.

**What this cell did NOT show, AS IT STOOD ON 2026-09-20.** Three of these
were superseded within hours by 2b.4, 2b.6 and 2b.7, and the list is kept in
the past tense rather than deleted because it is the record of what that
particular run established:

- Both stations were given **static** addresses. ~~The DHCP server still leases
  one hardcoded `192.168.99.2`~~ — superseded by 2b.4: the pool derives each
  address from the AID and two stations now lease `.2` and `.3`.
- **No station-to-station traffic.** Both pinged the AP, not each other.
  ~~The relay does not exist.~~ Superseded by 2b.7.
- **No group-addressed traffic.** ~~The GTK transmit path does not exist.~~
  Superseded by 2b.6.
- **Non-QoS only.** The AP advertises neither WMM nor HT, so this run says
  nothing about the 2b.1 nonce fix. **Still true of the on-air evidence** —
  but the fix is no longer interop-unproven: `tests/ccmp_kernel_vectors.h`
  pins it against sixteen QoS frames the Linux kernel encrypted, at all eight
  TIDs, and `ap_wpa2 --self-test` exercises the AP'''s own QoS receive path
  headlessly. What is still owed on air is a station sending TID 1..7, which
  rides with the WMM work.
- **Two stations, not seven**, and no churn: no station deauthenticated and
  re-associated while another held a key. **Still true.**

**Structural property to hold, stated because Phase 1's equivalent is what made
that phase checkable:** none of 2b.1-2b.7 may add a backend branch. All of it
is `src/sta/` or harness code. Only 2b.8 is allowed to be OS-specific, and that
is why it lives outside the library.

**Status:** 2b.1–2b.7 implemented and device-verified 2026-09-20; **2b.8 not
started**. The acceptance bar above is met. This line said "not started" while
sitting under that acceptance section, which is the contradiction the boundary
review's continuity angle caught — the same pattern rule 4 exists to prevent,
repeated inside the section written to correct it.

## Phase 3 — pure station logic, offline

The BSS table, the association state machine, the EAPOL/4-way supplicant, over
a crypto interface so `libdevourer` gains no dependency. No hardware.

**Acceptance.** `ctest` cells including two explicit negative cases, both of
them PR #335 defects that shipped in a reviewed PR: **a forged EAPOL-Key MIC
must be rejected**, and **an equal-counter replayed group rekey must be
rejected**. A phase that cannot fail those two tests has not been tested.

**Status: DONE 2026-09-21.** `264a6f7` built it, `03d2478` acted on the
review batch, `86d5ad5` added the cross-implementation vectors. ctest 78/78,
three new cells, none of which needs a radio, root or airtime.

| Module | What it is |
|---|---|
| `src/sta/Eapol.h` | the EAPOL-Key wire format, the 802.11 PRF, the PMK and PTK derivations, the MIC, the KDE walker |
| `src/sta/Supplicant.h` | the four-way and the group rekey, as a state machine |
| `src/sta/BssTable.h` | one record per BSSID, and which of them to join |
| `src/sta/StationSm.h` | authenticate, associate, four-way, connected — and every way that stops |
| `tests/openssl_crypto_ops.h` | the one COMPLETE CryptoOps in the tree; three partial ones were about to exist |

**The acceptance is met, and both cells carry their positive arm.**
`test_forged_mic_is_rejected()` asserts the negative across the verdict, the
counter, the absent message 4, the uninstalled PTK, the uninstalled GTK and
the unchanged state, then feeds the GENUINE message 3 and requires it to work
— so a supplicant that rejected everything cannot pass.
`test_group_rekey_replay_rejected()` does a genuine rekey, then the
byte-identical replay, then the case that actually matters (the same counter
carrying a DIFFERENT key) and requires the old GTK to survive, then a lower
counter refused outright and a greater one still installing.

**THE REVIEW FOUND A THIRD DEFECT OF THE SAME FAMILY, and it was mine.**
Message 1 of the four-way carries no MIC, and the first draft let it advance
the replay counter. Reproduced before fixing:

```
forged msg1 @2^64-1  verdict=Reply     replay_now=18446744073709551615
genuine msg1 @1      verdict=Replayed  replays=1
```

One frame anyone within range can build then refused every genuine EAPOL-Key
for the rest of the association — silently, because the station was already
keyed. It stayed Connected with `keyed()` true while its key management was
dead; the symptom would have appeared hours later as "multicast stopped
working". The counter now advances in exactly two places, both of which have
verified a MIC first. Two of the three reviewers found this independently.

**Nine more findings, all real and all fixed in `03d2478`:** `find_gtk_kde`
returning one bool for "absent" and "malformed" (a message 3 with a truncated
GTK KDE left the station Connected and keyed with no group key, nothing
counted); an unbounded transmit queue an attacker could fill from one captured
frame (measured at 12.1 MB of replies); `join()` not clearing that queue, so
frames for the BSS we left were aired at the one we joined; the SNonce being
an argument to `configure()` next to the PMK, when the two have opposite
lifetimes; `StationSm` counting nothing it dropped, on the one filter that on
real hardware is the only address filter in the system; `Connected` having no
exit but a deauth; the station advertising the AP's rate set, which omits the
mandatory 6 and 12 Mbps and invites status 18; keys not wiped on teardown,
which `docs/station-mode-scope.md` lists as a PR #335 review item and
`src/sta/StationTable.h` has done for the AP side since Phase 2b; and
`BssTable`'s eviction rule picking exactly the entry a flooding attacker wants
gone.

**Known answers, which is the class of test PR #335 had none of.** Two
independent sources, because one was not enough:

- the IEEE 802.11i Annex H.4.2 passphrase-to-PSK vectors, recomputed with
  Python's hashlib before being written down. They cover **PBKDF2 and nothing
  else** — an earlier comment implied more, which was the same kind of
  overstatement this phase exists to guard against.
- `tests/eapol_kernel_vectors.h`: the four EAPOL-Key frames **hostapd and
  wpa_supplicant actually exchanged**, captured off a two-radio
  `mac80211_hwsim` rig by `tests/eapol_capture_vectors.sh` — no hardware, so
  the bench is untouched. Our PTK must equal the one wpa_supplicant derived
  (the PRF and the address/nonce sorting), our MIC check must accept all three
  MIC'd frames and refuse message 3 under a KCK one bit out, and our
  Supplicant must drive the whole exchange to Done on the same PTK and
  hostapd's own GTK.

That last one **found two interop defects immediately**, both invisible to
every other test because both sit inside the MIC and our own authenticator
verified our own frames happily: messages 2 and 4 carried Key Length 16 where
802.11-2016 12.7.6.3 says 0 in an RSNA, and the 802.1X version octet was 2
where wpa_supplicant ships 1. With both fixed, our messages 2 and 4 are
byte-for-byte the ones wpa_supplicant sent.

**Two independent implementations, made to agree.** Each module's own test
uses a fixture written by the same author, so neither can catch a shared
misreading. `test_library_station_associates()` in
`tests/ap_wpa2_selftest.inc` runs this supplicant against the Phase 2b
authenticator — which hand-rolls every EAPOL offset inline, was written months
earlier, and shares no code with `src/sta/Eapol.h`. The station finds the AP's
own beacon in a `BssTable`, authenticates, associates, completes the four-way,
and both sides must hold the same PTK and GTK at the same key id.

**A third review batch, on `0dc7d70`, found one more real defect and six
assertions that could not fail.** `ccmp_aad` never masked the Order bit
(+HTC) on a QoS data frame, which 802.11-2016 12.5.3.3.3 requires and Linux
does — so every +HTC frame failed its MIC against a conforming peer, while
`data_hdr_len()` had always accounted for the HT Control field that bit
announces. The vector generator carried the same omission twice, which is why
no vector in it ever set the bit. The six unfalsifiable assertions are listed
in `2fef219`; the pattern worth carrying is that four of them passed because a
DIFFERENT rule produced the same outcome, so the rule under test could be
deleted with no test failing.

And running the selftests under `-DDEVOURER_SANITIZE=address+undefined` for
the first time — which happened only because one of the new arms depends on
that job — found a years-old overread in `test_aad_masking` itself: a 24-byte
array passed as a 26-byte QoS header. The suite is now green under the
sanitizer build too, 76/76.

**Mutations.** 28 on the first draft (27 caught; the survivor is recorded in
the code — deleting the "message 3's key data must be encrypted" check changes
no outcome, because plaintext key data fails the AES unwrap's integrity check
anyway), and 18 on the rules the review added, all caught. The harness now
refuses to run unless every anchor is present: killing it mid-run leaves a
mutation in the tree, and the next sweep then measures something that is
already wrong while looking plausible.

**Carried out of Phase 3, written down here rather than left for Phase 4 to
discover:**

- **No reconnect policy.** `StationSm` notices beacon loss and fails; it does
  not re-scan or re-join. Phase 4's `reconnect` cell has to drive that from
  outside, and the decision about *when* to retry is genuinely the
  integrator's.
- **No 802.11w.** A deauth is accepted unauthenticated, because without MFP
  there is no way to tell a forged one from a real one and a station that
  ignored them would stay associated to an AP that has forgotten it. Stated at
  the code, listed as out of scope in the scope document.
- **Nothing wires `BssTable` to `StationSm`.** A scanner — channels, dwell
  times, probe requests — needs a radio and is Phase 4's.
- **`aes_key_unwrap`'s output size is a contract `CryptoOps.h` does not
  state.** The caller sizes the buffer to `in_len - 8`, which is what RFC 3394
  writes, but the interface does not say so.
- **The four-way's interoperability is pinned offline, not on air.** The
  captured vectors and the cross-role cell are strong evidence; a real
  association against a real AP is Phase 5.

## Phase 4 — the harness

`tests/sta_client.cpp` and `tests/mt7612u_sta_onair.sh`, cells graded pass/fail
in the style of `mt7612u_ap_onair.sh`: `open`, `wpa2`, `reconnect`, `bench`.

**Acceptance.** The harness contains no backend branch — see "the end goal"
above; this is the structural test, and it is checkable by reading.

### Phase 4 — COMPLETE 2026-09-21

**Status: DONE.** `tests/sta_client.cpp` + `tests/sta_client_selftest.inc`
(ctest `sta_client_headless`, 17 cells) and `tests/mt7612u_sta_onair.sh`.
**15/15 on ch6** against hostapd on an RTL8812AU, `open` 5, `wpa2` 5,
`reconnect` 5; `bench` 2/2 separately.

**The acceptance property holds and is checkable by reading:** there is no
chip test anywhere in `sta_client.cpp`. The two places the silicon genuinely
differs go through the library — the trailing FCS via `tests/rx_mpdu.h`, and
the identity via `IRadio::SetStationIdentity` gated on
`AdapterCaps::station_mode_ok`.

**What the harness owns, which is exactly what Phase 3 wrote down that it was
refusing to guess at:** the scanner (`scan_step`/`probe`, feeding `BssTable`),
the reconnect policy (`supervise`), and which key the data plane uses per
frame. All three are now covered headlessly; none of them was before.

#### The library gap this phase found first: StationSm was WPA2-only

The plan has named an `open` cell since it was written, and there was no open
path — `configure()` derived a PMK unconditionally, `join()` refused any BSS
without `rsn_ccmp_psk`, and every association request carried an RSN element.
That is a missing *rung*, not a missing feature: on a WPA2 BSS association
and the key exchange come up together or not at all, so a station that fails
cannot say which half broke. The AP side has had the ladder since the
beginning (`ap_responder` / `ap_wpa2`). Added: `StationSm::configure_open`
(no PSK, no PMK, **no CryptoOps** — an open station links no crypto, exactly
as `ap_responder` does not) and `BssTable::select_open`, which is the second
function the comment above `select()` asked for. "Open" is `!privacy`, not
`!has_rsn`: a WEP BSS carries no RSN element and is not joinable either.

#### THE FINDING OF THIS PHASE: the group rekey arrives inside the cipher

hostapd, on the first WPA2 on-air run:

```
WPA: pairwise key handshake completed (RSN)
EAPOL-4WAY-HS-COMPLETED
WPA: group key handshake failed (RSN) after 4 tries
AP-STA-DISCONNECTED
```

`StationSm::on_rx` refuses every protected data frame under a comment reading
"the four-way is never protected: the keys it carries are what protection
would need". That sentence is true, and it is about the **four-way**. The
**group key handshake** runs *after* the PTK is installed and is protected
like any other data frame, so the refusal swallowed the entire handshake —
all four of hostapd's message 1s landed in `rx_ignored`. A station that
cannot answer a rekey is thrown off the BSS by every AP that performs one,
and **the link reports itself healthy right up until it ends**: the first run
read `associations=2, reconnects=1` in thirty seconds with every crypto
counter at zero.

Not reachable through this project's own AP, which never sends a group
message 1 — so nothing in this tree could have found it. This is the single
strongest argument in the whole workstream for the independent-witness rule
that Phase 5 states and `docs/mt7612u-ap-mode.md` admits it broke.

Fixed by splitting framing from decision: `StationSm::eapol_reply()` returns
the EAPOL-Key **body**, and `on_decrypted_msdu()` is the entry for a frame
the caller has already unprotected. The reply is a body and not a frame
because the two callers need different framing — the four-way's answer is
cleartext and the machine can build it, a rekey's must be encrypted and the
machine holds no cipher. Measured after: *"group key handshake completed
(RSN)"* twice in 75 s, one association, `reconnects=0`.

**And the cell that covered this was green throughout.** `test_group_rekey`
fed message 1 in *cleartext*, because that is the shape the fixture already
built — so it exercised the four-way's path and left the one that matters
unreached. That is this phase's contribution to the "which assertion could
not fail" ledger, and it is a sharper form than the earlier ones: the cell
did not merely pass for another reason, it tested **a different code path
than its name claims**.

#### Four more defects, each found by something other than reading

- **The re-join backoff was armed on the first pass through `supervise()`**,
  so a freshly started station sat idle for a full second before it would
  even look at the scan table. Found by the headless cells before any
  hardware; on a bench it would have read as "the AP is slow to answer".
- **`GetPermanentMacAddress` was called before `InitWrite`**, where the
  device handle does not exist yet. Reported as *"the radio does not report
  its MAC address"*, which names the wrong thing entirely.
- **The startup `SetStationIdentity(own, own)` is refused by contract** —
  the two must differ. It looked like a capability failure and was a harness
  bug. The identity is armed once, for the BSS actually joined.
- **`rx_ignored` was counting every protected data frame**, which on a
  working link is all of them: a 60-second run carrying 75 frames reported
  `ignored=75`, reading as 75 protocol errors. That counter set is the *only*
  thing that answers "why did nothing associate" on a promiscuous receiver,
  so it now has its own `rx_protected`.
- **`reconnects` counted retries, not losses.** An on-air run that dropped
  the link exactly once, with the AP off for eight seconds, reported
  `reconnects=6`. The first latch has to be cleared by every join attempt or
  a second loss could never be noticed, so it counts attempts; a second latch
  set at association makes the number mean what its name says. Now `1`.
- **The bench cell passed while measuring nothing.** `ccmp.profile` reported
  `tx_frames=0` over 3365 encrypted round trips: the profiling `CryptoOps`
  was written and never instantiated, and the only graded check was the reply
  count, which says nothing about whether the cipher was timed. The cell now
  fails on a zero-frame profile.

#### Mutations

11 on the open-network path (10 killed), 20 on the harness (20 killed, after
two cells were rewritten because they could not fail — see `580aff5`). **One
recorded survivor**, stated at the line rather than implied by its absence:
`configure_open`'s `secure_wipe` of the PMK is unobservable, because
`have_pmk_` is cleared either way. It is defence against a core dump, not
against a caller.

And a **harness bug worth carrying**: the first sweep restored files with
`shutil.copy2`, which preserves the mtime — so the restored file looked older
than the object built from the mutated one, make never rebuilt it, and the
"clean" tree afterwards still ran a mutated binary. It came back as seven
failures in a suite that had just been green. Suspect the build before the
test; see `feedback-a-hand-maintained-dependency-list-yields-false-passes`.

#### What the on-air harness does that the AP one does not

**The AP lives in a network namespace.** Both radios are on one host, so with
both interfaces in the root namespace and both addresses in one subnet the
kernel routes between them *locally* and the ping never touches the air — a
data-plane cell that passes with the antennas unplugged. The PHY is moved
with `iw phy <phy> set netns` (a cfg80211 interface cannot be moved with
`ip link set netns`), and every cell asserts `ip route get` before measuring.

**And `ip netns del` on a namespace that still holds the PHY destroys it.**
Measured by doing exactly that: the USB device stayed bound and enumerated,
`/sys/class/ieee80211` lost the phy entirely, and only a bus re-enumeration
brought it back. The delete is conditional on the move having worked.

#### Numbers, with their adversarial counterpart

`bench`, ch6, 1400-byte payload, 15 s, MT7612U station against hostapd:

| | |
|---|---|
| replies | 3340/3377, 1.1% loss |
| station CPU | 3.67% of a core (incremental over a paired idle window) |
| system | 0.168 cores incremental |
| `ccmp_tx_ns_per_frame` | 5915 |
| `ccmp_rx_ns_per_frame` | 15143 |

**What this is not.** A flood ping is round-trip bound, so `reply_pps` (223)
is a *latency* figure and not link throughput — nothing in this tree measures
throughput. The receive path costs 2.5× the transmit path per frame and
**that asymmetry is unexplained**; it is reported because it was measured,
not because it is understood.

#### Carried out of Phase 4

- **No 802.11w still.** Unchanged from Phase 3, and now it has an on-air
  consequence worth naming: the `reconnect` cell stops the AP rather than
  sending a deauth, partly because a deauth exercises a path that needs no
  supervision at all — and partly because an unauthenticated deauth is
  exactly what this station cannot tell from a forged one.
- **The channel sweep is written but not exercised on air.** Every cell runs
  on one configured channel, which is what `DEVOURER_STA_SCAN_CHANNELS`
  defaults to. `scan_step`'s multi-channel branch has a headless cell and no
  on-air one; retuning under a live association is refused by construction
  (`supervise` returns the joined channel once the machine leaves
  Idle/Failed) and that construction is untested against a real retune.
- **One band.** ch6 only: the RTL8812AU's 5 GHz channels are all `no IR` in
  this regulatory domain, so hostapd cannot serve them here. The AP harness
  reads 14/14 on ch36 because *devourer* airs the beacon there; this harness
  needs a kernel AP and therefore needs a different regdomain or a different
  AP adapter to reach 5 GHz.
- **`aes_key_unwrap`'s output-size contract** is still unstated in
  `CryptoOps.h`. Carried forward unchanged from Phase 3.

## Phase 5 — validation, independent witness first

Against **hostapd on non-MediaTek silicon** before anything else.
`docs/mt7612u-ap-mode.md` states its own weakness plainly — its station was the
same silicon, so it was not an independent-generation witness. The station work
should not repeat that. Then devourer-to-devourer against `ap_wpa2.cpp` on a
second adapter, which is the FPV shape. Then throughput and latency, each
number with its adversarial counterpart in the same breath.

**Status: the independent-witness half is DONE, ahead of schedule and by
accident.** Phase 4's harness needed *something* to associate to in order to
have any cells at all, and the honest choice was hostapd on non-MediaTek
silicon rather than this project's own AP — so `tests/mt7612u_sta_onair.sh`
already IS the independent-witness run, 15/15 on ch6, and it found the group
rekey defect on its first attempt. What Phase 5 still owes:

- **devourer-to-devourer**, MT7612U station against `ap_wpa2.cpp` on a second
  adapter. That is the FPV shape and it is the one this project ships.
- **5 GHz**, which this bench cannot serve from a kernel AP (see Phase 4's
  carried-forward list).
- **Throughput**, which nothing in this tree measures yet, and latency under
  load. Each with its adversarial counterpart in the same breath.
- **A soak.** Every figure above comes from runs of 45–75 seconds.

## Phase 6 — the Realtek arm, deferred to its own issue

`SetStationIdentity` on Jaguar1/2/3, with PR #335's `src/apfpv/StationMode.cpp`
as the reference (the kernel `hw_var_set_opmode` STATION path) plus its
`SetStationRxFilter`. Its close-range DIG-saturation finding is worth landing
independently of the rest.

This phase is the proof of the structural claim: if it is only an `IRadio`
implementation and the harness is untouched, the seam was right.

**Status:** not started. Deliberately after a working MT7612U station, not
alongside it.
