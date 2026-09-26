# Station mode — where the work stands, and how to pick it up

Companion to `docs/station-mode-scope.md` (the technical scope),
`docs/station-mode-plan.md` (phases, gates, the review ledger) and
`docs/station-mode-phase0.md` (the Phase 0 measurement). This file is the
session-continuity note: what exists, where it lives, how to run it, and what
the next person should distrust.

## Location

Worktree `.claude/worktrees/mt7612u-station-scope`, branch
`worktree-mt7612u-station-scope`, based on `origin/master` at `14a4881`.

**Pushed to the FORK only**, as
`fork/feature/mt7612u-station-mode`, with a draft PR at
<https://github.com/snokvist/devourer/pull/3>. `origin` is **OpenIPC/devourer**
— upstream — so a bare `gh pr create` targets the wrong repository. Nothing
goes upstream until the whole workstream is finished.

```sh
git push fork HEAD:refs/heads/feature/mt7612u-station-mode
```

The commit list below stops at Phase 1; `git log --oneline` is the current
one.

```
082c89d tests: adversarial review round 5 - two gate checks could not fail
e26d12c tests: the AP acceptance harness passes 14/14 on an independent station
5d2af14 docs: station-mode handoff - state, rig, and what to distrust
63a618c sta: review round 4 - the replay reset was defeatable, and I overclaimed
112d7cc sta: ccmp_encrypt bounds its output buffer
22e0518 sta: harnesses derive the header length instead of testing fc0 == 0x88
224a544 sta: close the pre-Phase-3 open items, and the data plane works on air
68b035c plan: Phase 1 on-air result, and the control that made it readable
e26e62e sta: explicit header length in the CCMP API, and honest vectors
e035064 sta: review round 2 - parser state, QoS subtypes, golden bytes, one RSN
1a51484 sta: share the management-frame module, and switch both AP harnesses onto it
25cbd76 sta: the CCMP framing as a shared, known-answer-tested module
215f7bb mt7612u: read the retry count off the chip; the No-Ack cliff is not retries
1139782 mt7612u: land the software-CCMP benchmark, rebased onto master
b8c37f6 Phase 0: the unicast cliff is TX-only injection, not a station — gate PASS
b5aa127 station mode: scope, gated plan, and the Phase 0 unicast measurement
```

**`bench/mt7612u-ccmp` is a decoy.** It is a label six commits behind master
with no commits of its own; everything it appeared to hold was uncommitted
working-tree state in the primary checkout. `1139782` landed that content here,
rebased. The originals are still sitting in `/home/snokvist/dev/devourer` —
untouched, because they were copied rather than moved. Decide whether to clean
that tree up; nothing here depends on it any more.

## State by phase

| Phase | State |
|---|---|
| 0 — can the part carry a station? | **PASS.** The 40× unicast cliff is a property of TX-only injection with the MAC receiver disabled, not of the part. Station-shaped TX runs at 2084 fps / 99.9% ACK. |
| 1 — shared frame + crypto layer | **DONE, GATE CLOSED.** 14/14 twice on each band against independent silicon. See below. |
| 2 — the `IRadio` seam | **Implemented.** Seam + caps flag + MT7612U implementation + five bring-up gates + a headless selftest. R5 and R6 both measured; `station_mode_ok` is **true** for MT7612U. `docs/mt7612u-station-identity.md` — read its retraction section before quoting any number. The R6 table was re-taken 2026-09-20 under the corrected single-variable harness and holds. |
| 3 — pure station logic | **DONE 2026-09-21.** BSS table, association state machine, EAPOL/4-way supplicant, all headless. Both acceptance negatives present and load-bearing. Pinned against a captured hostapd/wpa_supplicant four-way. |
| 4 — the harness | **DONE 2026-09-21.** `tests/sta_client.cpp` (+ its `.inc`, ctest `sta_client_headless`) and `tests/mt7612u_sta_onair.sh`. **16/16 on ch6** against hostapd on an RTL8812AU; `bench` 2/2 separately. No backend branch anywhere in it, which is the phase's acceptance property. |
| 5 — validation | **DONE 2026-09-26 on 5 GHz; two 2.4 GHz gates open.** Close-out review done (Round 6 + its addendum; every finding resolved). The d2d link now runs with retransmission at both ends by default - `AP_RETRY=3` (Realtek descriptor retry limit), `STA_ACK=1` (the station had sent every frame NOACK and never retried), `STA_RETRY=5` (MT7612U MAC retry limit) - plus 802.11 duplicate detection at both ends. With them, ch36 MCS7: 0.00% loss both ways to 20 up / 30 down Mbit/s; 6M 0.00%; `all` 21/21 (every ping cell 20/20, the old flaky 6M/ch6 cell included); a 4-min bidirectional soak 0.00% with `replays rejected` 0. The 30-min soak (8812CU) and 15-min soak (8812BU) ran BEFORE the defaults and passed 5/5 with ~3% downlink loss; the 8812CU one was RE-RUN with the defaults 2026-09-26: 5/5, uplink 0.00% in all 30 chunks, downlink 0.00-0.02%, one association, 643k frames each way with both ledgers closed, replays 0, AP RSS flat (11928 -> 11940 kB, sampled every 30 s) - one run. 2.4 GHz, re-run with the defaults 2026-09-26: the 8812BU as AP on ch6 now PASSES `thru` (it failed before retries) - uplink 0.00% to 20 Mbit/s, downlink 0.67-3.10% per rung against 0.12-0.34% for the 8812CU AP on the same channel, same station, back to back (one run each: the 8812BU's excess is the AP adapter or Jaguar2, not the channel). The 8812AU as AP on ch6 (re-run 2026-09-26, adapter at 6-1) failed a NEW way: the downlink collapsed (the station heard ~100 of our beacons in 10 min, 55 reconnects). An independent witness (devourer `rxdemo` on the 8812CU, filtered to our BSSID) caught 82 of our frames on ch6 against 1109 on ch36 from the same AP - the 8812AU was DEFERRING, not weak (RSSI 94-99 when it aired). Single-gate split via the new `DEVOURER_AP_CCA_GATES` diagnostic: EDCCA off alone 1240 frames, primary CCA off alone 472, both off 1138 - **EDCCA**, the family's documented gate, tripping on 2.4 GHz energy at this placement (the source - the adjacent MT7612U, the bench's MediaTek Bluetooth radio - is not separated). With EDCCA off, `thru` PASSES: up 0.00% to 20 Mbit/s (the old flat 18-21% uplink loss is gone with the station retrying), down 0.65-1.56% to 14 Mbit/s, saturating ~16. One run per arm. EDCCA stays ON by default (standards-compliant); `AP_RETRY` was ruled out first (unset: the same failure). The Jaguar3 AP TX wedge (REG_CR PROTOCOL_EN at the LLT init) is fixed on 8822C/8822E and the same defect on Jaguar2 (8822B): `docs/jaguar3-tx-ring.md`. |
| 6 — the Realtek arm | **Ported on Jaguar1/2/3, MEASURED on the 8822C and 8822B, 2026-09-26.** `src/StationArm.h` over `AckResponder.h`: MACID = own, BSSID = the AP, net_type = Infra, exact pre-arm restore, refused while a beacon or ACK responder owns port 0 (and they refuse while it is armed). `station_mode_ok` true on those two dies only. The structural test held: `sta_client` needed no backend branch - the harness gained `STA_VID/STA_PID` (the station was hardcoded MT7612U) and the `STA_ARM=0` control. Review round 7 (two reviews, converged; `docs/station-mode-plan.md`) fixed the Jaguar2/3 port-0 ownership holes it found. 8812CU station: `wpa2` 8/8; `thru` armed vs unarmed 2 vs 42122 station duplicates (2.98x delivered = every downlink frame aired AP_RETRY+1 times unarmed); 8812BU station 13 vs 35117 (2.99x). **One caller bug found and fixed**: `sta_client` armed under its RX lock and deadlocked (sync USB I/O waits on the RX thread) - IRadio's `StartRxLoop` now states the rule. Jaguar1 8812AU measured 2026-09-26 (flag TRUE on CHIP_8812): armed 38 / unarmed 27 duplicates - the control does NOT discriminate there, as predicted (bring-up puts `own` in MACID and the 8812 MACID answers with net_type NoLink); the station behaviour holds either way, 2/2 `thru`, clear verified. Open: the 8822E (needs an 8812EU) and 8821C (no cell). |

## What exists now

Pure, backend-neutral, under `src/sta/` — no device, no environment, no clock,
no threads, no sockets:

- `CryptoOps.h` — the crypto vtable, so `libdevourer` gains no OpenSSL.
- `Ccmp.h` — 802.11 CCMP framing: AAD (incl. QoS TID and 4-address A4),
  nonce, CCMP header, PN, encrypt/decrypt, per-TID replay window.
- `Dot11.h` — management frames: header builder, IE builders, a bounds-checked
  IE walker, RSN build **and parse**, beacon/auth/assoc parsers,
  station-side request builders (with the STATION's own rate set, which the
  AP's deliberately is not), sequence counter, data-frame headers, and the
  802.11 ↔ 802.3 translation.
- `StationTable.h` — the AP's per-station records, and `decide_forward()`.
- `Eapol.h` — the EAPOL-Key wire format, the 802.11 PRF, the PMK and PTK
  derivations, the MIC, the KDE walker.
- `Supplicant.h` — the four-way and the group rekey, as a state machine.
- `BssTable.h` — one record per BSSID, and which of them to join.
- `StationSm.h` — authenticate, associate, four-way, connected, and every way
  that stops. **Open networks too, since Phase 4** (`configure_open`), which
  the plan had always required and the module had never had.

And the harness that drives them, which is deliberately NOT library code:

- `tests/sta_client.cpp` — devourer as a station. The scanner, the reconnect
  policy and the per-frame key selection, all three of which Phase 3 wrote
  down that it was refusing to guess at. **It contains no backend branch**,
  which is Phase 4's whole acceptance test and is checkable by reading.

Tested headless by `ctest`: `ccmp_framing`, `dot11_frames`,
`ccmp_software_roundtrip`, `station_table`, `ap_wpa2_headless` — which runs
the WPA2 AP harness itself with no radio — `supplicant`, `station_sm`,
`bss_table`, and since Phase 4 `sta_client_headless`, which runs the STATION
harness with no radio against a fixture authenticator. **79/79 green, and
77/77 under `-DDEVOURER_SANITIZE=address+undefined`.** Every selftest here was
verified capable of failing by injecting the defect it exists to catch.

`supplicant` carries `tests/eapol_kernel_vectors.h` in the same spirit: the
four EAPOL-Key frames hostapd and wpa_supplicant actually exchanged, with the
PTK wpa_supplicant derived and the GTK it installed, captured by
`tests/eapol_capture_vectors.sh`. It is the only thing in the tree that pins
the PRF, the EAPOL MIC and the GTK KDE layout against software that has never
read this repository — and it found two interop defects on its first run.

`ccmp_framing` also carries `tests/ccmp_kernel_vectors.h`: sixteen protected
QoS data frames the LINUX KERNEL encrypted, all eight TIDs in both directions,
captured off a two-radio `mac80211_hwsim` rig by
`tests/ccmp_capture_vectors.sh` (no hardware, no bench). They are the
independent check the generated vectors are not — generator and header share
one reading of the spec, which is how a CCM nonce with a zero flags octet
stayed green through the whole suite.

Both AP harnesses (`tests/ap_responder.cpp`, `tests/ap_wpa2.cpp`) are switched
onto these modules. That is the load-bearing property: a station module only a
station used would be a station module with AP-shaped holes.

MT7612U-specific, in the same branch: `MT_TX_STAT_FIFO` declared and
`MT_TXOPT_TXS` added so the chip reports its own retry count
(`bringup txs`, `docs/mt7612u-tx-retry.md`), plus `bringup ucast` (`gate_ucast`)
for the Phase 0 measurement.

## The rig, and how to reproduce the on-air cells

**Sysfs paths move between sessions. Check them first** (`lsusb -t`, or the
`idVendor`/`idProduct` under `/sys/bus/usb/devices/*`); every recipe below
takes them as arguments precisely because they are not stable. As of
2026-09-21 on this bench: MT7612U at `1-1`, RTL8812CU at `5-1`, RTL8812AU at
`8-1`.

### The AP harness — devourer SERVES, a kernel driver joins

| role | part | notes |
|---|---|---|
| AP | MT7612U `0e8d:7612` | firmware from `/lib/firmware/mediatek` |
| station | RTL8812AU `0bda:8812` | the **in-tree rtw88** driver: module `rtw88_8812au`, sysfs driver name `rtw_8812au` (both spellings are in this tree; they name different things). **Independent silicon**, which is the witness `docs/mt7612u-ap-mode.md` says it lacked |

```sh
sudo AP_SYSFS=1-1 STA_SYSFS=8-1 CH=6  FW_DIR=/lib/firmware/mediatek tests/mt7612u_ap_onair.sh all
sudo AP_SYSFS=1-1 STA_SYSFS=8-1 CH=36 FW_DIR=/lib/firmware/mediatek tests/mt7612u_ap_onair.sh all
```

### The station harness — devourer JOINS, hostapd serves

The roles are **swapped**, and so are the variable names: `STA_SYSFS` is the
MT7612U devourer claims and `AP_SYSFS` is the kernel-driven adapter hostapd
drives. Getting these the wrong way round is the first mistake to check.

```sh
sudo STA_SYSFS=1-1 AP_SYSFS=8-1 CH=6 FW_DIR=/lib/firmware/mediatek \
     tests/mt7612u_sta_onair.sh all        # open + wpa2 + reconnect = 16 checks
sudo STA_SYSFS=1-1 AP_SYSFS=8-1 CH=6 tests/mt7612u_sta_onair.sh bench
```

Three things it encodes that are not obvious:

1. **The AP runs in a network namespace.** Both radios are on one host, so
   with both interfaces in the root namespace and both addresses in one
   subnet the kernel routes between them locally and the ping never touches
   the air — a data-plane cell that passes with the antennas unplugged. The
   PHY is moved with `iw phy <phy> set netns`; a cfg80211 interface cannot be
   moved with `ip link set netns`. Every cell asserts `ip route get` first.
2. **`ip netns del` on a namespace that still holds the PHY DESTROYS it.**
   Measured: the USB device stays bound and enumerated, the phy disappears
   from `/sys/class/ieee80211` entirely, and only a bus re-enumeration brings
   it back. The script moves the phy out first and refuses to delete the
   namespace otherwise. If you are ever left with a vanished adapter:
   `sudo sh -c 'echo 0 > /sys/bus/usb/devices/8-1/authorized'` then `1`.
3. **ch6 only.** The RTL8812AU's 5 GHz channels are all `no IR` in this
   regulatory domain, so hostapd will not serve them here. The AP harness
   reaches ch36 because *devourer* airs that beacon; this one needs a kernel
   AP.

That is the whole recipe now. **`tests/ap_onair_rtl_sta.sh` is gone** — every
check in it was strictly weaker than the equivalent above (substring SSID
match with no BSSID or frequency, "AP up" taken from a banner that prints
`beacon=FAIL` unconditionally, `iw connect`'s exit status discarded, power
save never touched, an unguarded `authorized` write to a caller-supplied
sysfs path). It existed only because the acceptance harness could not drive a
non-MediaTek station, which is no longer true. Earlier revisions of this file
named it as *the* reproduction recipe; it could not even run in its committed
form until `e26d12c` fixed its `env` invocation.

Things the harness now encodes that each cost a run to learn:

1. **Station power save must be off**, and the harness enforces it. This AP
   cannot serve a power-saving station at all — see below.
2. The station must be **re-enumerated** (authorized toggle), not driver-bound,
   if its netdev is missing: devourer's libusb claim leaves the interface on
   `usbfs`, and writing a driver's `bind` reports success without re-probing.
   The netdev can also vanish *mid-run* if another process claims the adapter;
   `sta_check()` recovers it and says so rather than scoring it as an AP result.
3. `BCN_TU` defaults to 100 and **that is fine** — see the withdrawn claim
   below.

To free the MT7612U from the kernel: `echo 7-1:1.0 > /sys/bus/usb/drivers/mt76x2u/unbind`.

### The devourer-to-devourer harness — no kernel 802.11 at either end

`tests/sta_d2d_onair.sh`. `STA_SYSFS` is the MT7612U running `sta_client`;
`AP_SYSFS` is the adapter running `ap_wpa2` — **an RTL8812CU**, because that
is the PID `tests/ap_wpa2.cpp` defaults to and the generation its AP path is
validated on.

```sh
sudo tests/sta_d2d_onair.sh              # wpa2 + fiveghz + airgap = 21 checks
sudo STA_SYSFS=1-1 AP_SYSFS=5-1 CH=6 CH5=36 tests/sta_d2d_onair.sh all
sudo tests/sta_d2d_onair.sh bench        # paced; 3 checks
sudo tests/sta_d2d_onair.sh flood        # the ceiling + both ledgers; 3 checks
```

What is different from the hostapd harness, and why:

1. **5 GHz works here.** devourer programs its own synthesizer regardless of
   regulatory domain, so `fiveghz` (ch36) reads the same 8/8 as ch6. The
   operator owns compliance.
2. **The netns is cheap and safe.** Only the AP *process* goes into it, so the
   only thing in the namespace is a TAP that dies with the process. There is
   no phy to move and therefore no phy to destroy — the trap the hostapd
   harness spends thirty lines guarding against does not exist here.
3. **The AP's TAP must carry `ap_wpa2`'s BSSID** (`02:42:75:05:d6:00`), or
   frames to it take the off-BSS branch instead of the "for this AP" one. It
   still works that way, which is exactly why a cell checks the address: the
   station prints the BSSID it armed, and the harness asserts it matches.
4. **`airgap` is the falsifier.** Radios on different channels (at least
   40 MHz apart), same plumbing, and the ping must be 100% lost. Run it after
   any change to the netns or addressing, because it is the only thing that
   proves the other cells could fail. It carries its own positive control -
   the station must have heard `BEACONS_MIN` beacons from *somebody*, because
   a deaf receiver gives this cell its answer for the wrong reason.
5. **The data-plane cells tolerate loss, deliberately.** 16 of 20 delivered,
   not 6 of 6. Neither end retransmits (see item 9 of the open list), the
   underlying rate is ~5% per round trip on both bands, and a zero-loss
   threshold over six packets was a coin toss. A dead data plane still reads
   100%.
6. **Both ledgers state two exact identities each**, and the `flood` cell
   checks all four: `from host == framed + dropped down` and `queued == aired
   + queue dropped + send failed`. `test_the_books_close` in
   `tests/sta_client_selftest.inc` pins them headlessly.

Two numbers to expect, and neither is throughput: a paced 10 pps of 1400 B
loses ~27% on ch6 and ~4–5% on ch36 (the band, not the link — neither end arms
`SetAckResponder`, so nothing is retransmitted), and a flood ceilings at ~11–22
round trips a second on **both** bands, because the station drops the
association under load. See the plan's Phase 5 section for what that has been
narrowed to and what it has not.

## What the on-air runs actually showed

`tests/mt7612u_ap_onair.sh` reads **14/14 twice on 2.4 GHz (ch6) and twice on
5 GHz (ch36)**, at the default `BCN_TU=100`. Beacon armed and scannable, open
auth+assoc, hardware auto-ACK, an ARP/ICMP data plane, the WPA2 4-way with a
verified MIC, an encrypted data plane, and the full beacon lifecycle
(arm → stop → re-arm) on both bands. RTT 0.7–2.4 ms in every cell.

The adversarial counterpart, in the same breath, because this tree's rules
require it:

- **Power save is forced off, and this AP cannot serve a power-saving station
  at all.** None of the three beacon builders appends a TIM element. **Superseded:** the AP harness beacons now carry a TIM (`append_tim`, `src/sta/Dot11.h`); `tests/mt7612u_beacon_stop_check.cpp` still does not. Buffering is still absent, so power save is still unsupported — that half stands. Linux
  defaults to `power_save on`, so 14/14 does *not* certify this AP against a
  default-configured Linux client.
- The 5 GHz open cell's data-plane check **failed once in three runs** on a
  single lost ping — 6 packets at 0% loss on a channel with several strong
  neighbours. Brittle by construction; recorded, not loosened.
- n=2 per band, one AP unit, one station unit, near field, nothing soaked
  (longest run about two minutes).

## What to distrust

- **Power save is the single biggest caveat on the 14/14.** It is forced off,
  and this AP buffers nothing, so it cannot serve a power-saving station —
  which is what Linux defaults to. The score is real and its scope is narrower
  than it looks. (The beacons DO carry a TIM now; that fixed the conformance
  gap, not the buffering one, and the 0/60 measurement predates it.)
- **"The link is unstable" and "RTT is poor" (524 ms mean, 1729 ms max) were
  probably power save, and that is inference from ONE ordered pair of runs.**
  PS on gave 0/60 pings, PS off gave 60/60 at 0.735/1.530/7.644 ms. Note what
  that pair does *not* do: it never reproduces the 524 ms RTT in either arm —
  the PS-on arm received nothing at all — and it cannot by itself explain a
  population claim like "half the runs carry no packet". Post-fix RTT is
  0.7–2.4 ms everywhere, so something real changed. Do not write it up as
  settled.
- **The AUTH-at-retry=1 asymmetry is unexplained.** Five runs: AUTH arrived
  retried in four, ASSOC at retry=0 in all five. The new auto-ACK witness
  reads the station's per-peer counters, which are created at association, so
  it says nothing about the AUTH that precedes them. Still open.
- **Nothing is soaked.** Longest run about two minutes; n=2 per band.
- **The sequence-number causality is inference.** The data plane went from
  100% loss to 0% when data-frame sequence numbers landed, and the QoS AAD fix
  had already shipped before the failing run — so by elimination it was the
  sequence number. That is an ordered pair of runs, not an A/B. And with power
  save now known to produce exactly this symptom, that elimination has one
  more untested alternative in it than it did when it was written.
- **5 GHz on this station is `no IR` on every channel**, yet association works
  and the earlier "-22 from `iw connect`" did not recur. What changed is not
  established; the harness now pins frequency and BSSID on connect, which
  makes the cell deterministic without explaining it.
- **A second cfg80211 cache entry for our own BSSID** appears while the AP is
  up, carrying a frequency we are not airing on and an SSID-only IE set. Its
  origin is NOT established. Pinning the BSSID changes which BSS the *station*
  targets and changes nothing about what the AP transmits — so if that entry
  is produced by something this AP airs, the harness now passes without
  surfacing it.
- ~~**The ledger lies on Realtek.**~~ **RETRACTED 2026-09-23.** It said
  `on_rx` does not trim the trailing FCS, so a Realtek AP would report a
  length bug as MIC failures. The trim went in with `tests/rx_mpdu.h` and this
  entry outlived it. `tests/ap_wpa2.cpp` has since been run as the AP on an
  RTL8812CU with a MediaTek station decrypting every frame — 0 MIC failures
  across every `sta_d2d_onair.sh` run. The comment saying the same thing in
  the source is gone too; a warning about a fixed bug is worse than none,
  because a reader trusts it.

## What the station's on-air runs showed

`tests/mt7612u_sta_onair.sh` reads **16/16 on ch6** against hostapd on an
RTL8812AU — the MT7612U authenticates, associates, runs the four-way as the
supplicant, answers group rekeys, carries an encrypted data plane, notices
the AP going away and re-joins by itself when it comes back.

**The first WPA2 run failed, and that is the most valuable thing in this
section.** hostapd logged *"group key handshake failed (RSN) after 4 tries"*
and threw the station off the BSS. `StationSm::on_rx` refused every protected
data frame — correct for the four-way, which runs before there is a key, and
wrong for the group key handshake, which runs after the PTK is installed and
is protected like any other data frame. Nothing in this tree could have found
it: this project's own AP never sends a group message 1. See the Phase 4
section of `docs/station-mode-plan.md`.

Numbers, with their counterpart in the same breath (`bench`, 1400 B, 15 s):
3340/3377 replies at 1.1% loss, 3.67% of a core incremental, CCMP 5915 ns per
transmitted frame and 15143 ns per received one. **`reply_pps` is a LATENCY
figure** — a flood ping is round-trip bound — and nothing in this tree
measures throughput. The 2.5× receive-over-transmit asymmetry is reported
because it was measured, not because it is understood.

## Open, carried forward

1. **The ~20 ms No-Ack unicast cost.** `docs/mt7612u-tx-retry.md`: the MAC
   completes these on the first attempt with 0 retries and 100% success, and
   they still cost ~20 ms. So it is **not** the retry ladder — this branch
   refuted its own earlier hypothesis. Arms for WCID, QSEL and A-MPDU are in
   `bringup txs` and **have not been run**. Matters for injection, not for a
   station.
2. ~~**`CcmpReplay` is a strict counter, not a bitmap.**~~ **CLOSED** by
   `f0a67b7`: it is a 64-slot sliding window with a per-TID mask across 17
   slots (`src/sta/Ccmp.h`). Reorder within the window is tolerated and an
   equal or already-seen PN is still rejected. Note that R3's "decline ADDBA"
   recommendation was propped partly on the old no-reorder-tolerance argument,
   so that recommendation should be re-derived rather than inherited.
3. **The KAT vectors pin the cipher plumbing, not the framing rules.** Stated
   plainly in `tests/ccmp_gen_vectors.py`. The IEEE 802.11-2016 Annex J CCMP
   vector is a drop-in improvement and the selftest is shaped for it.
4. **`SetStationIdentity` is provisional.** It ships with one implementation;
   `docs/station-mode-scope.md` argues the position honestly and Phase 2's gate
   should weigh landing the Realtek arm alongside it.
5. **The channel sweep is not exercised on air.** Every station cell runs on
   one configured channel. It has a headless cell only since the Phase 4
   review round - the claim that one existed was false when it was written -
   and the rotation has never met a **real** retune, which on this part takes
   48-526 ms.
6. ~~**No duplicate filter on the station's receive path.**~~ **CLOSED
   2026-09-26**: both ends now run 802.11 duplicate detection before decrypt
   (`sta::DupDetector`, Retry bit + Sequence Control per TID; unicast only at
   the station), so it covers open links too. It became necessary, not
   theoretical, once both ends retried: a lost ACK had the copy counted as a
   replay and the `wpa2` cell's ledger check failed on it.
7. **No 802.11w**, unchanged, and now with an on-air consequence: the
   `reconnect` cell stops the AP rather than sending a deauth, partly because
   an unauthenticated deauth is exactly what this station cannot tell from a
   forged one.
8. ~~**The station drops its association under load, and why is not known.**~~
   **RESOLVED 2026-09-24 - it was never the station.** The Jaguar3 AP's TX
   data ring ran into the reserved region and overwrote its own beacon page;
   the next TBTT latched a TX-DMA fault and the AP stopped transmitting
   (beacons and management replies both), so the station's supervision
   correctly declared the link dead. Fixed 2026-09-25 (`REG_CR`, superseding `222bcf9`); record and the
   remaining Jaguar3 items in `docs/jaguar3-tx-ring.md`.
9. ~~**No link-layer retransmission anywhere on the devourer-to-devourer link.**~~
   **RESOLVED 2026-09-25, and the premise was half wrong.** The UPLINK already
   has it: the station's MT7612U requests an ACK with a 15-deep retry and the
   Realtek AP already ACKs it (zero replays over 321k soak frames), so
   `SetAckResponder` (`ARQ=1`) measured as no change. The DOWNLINK had none:
   the AP airs data with the library default retry limit 0. `AP_RETRY=7`
   (`DEVOURER_TX_RETRY_LIMIT`) took downlink loss from 1.3-2.3% to 0.00% on
   every rung up to 30 Mbit/s at ch36 - ONE ladder per arm, ch36 only; the
   ch6 conditions behind the old 27% are untested with it, and the library
   and harness default is still 0, so a default d2d downlink still has no
   retransmission. The UPLINK had none either: the station sent every
   frame radiotap-NOACK, so its MT7612U never retried - `STA_ACK=1`
   (`DEVOURER_STA_ACK`) requests ACKs on unicast and took a 6M uplink to
   0.00%. (An "ACKed-but-undelivered at the AP" reading of the zero witness
   retries was that NOACK, and is retracted.) Table:
   `docs/jaguar3-tx-ring.md` items 5 and 6.

10. **The 2.4 GHz gates, re-measured with retries.** HALF DONE 2026-09-26:
    the 8812BU-as-AP ch6 `thru` now passes (up 0.00% to 20 Mbit/s; down
    0.67-3.10% vs the 8812CU AP's 0.12-0.34% on the same air, back to back -
    one run each, so the 8812BU's downlink excess is the adapter or Jaguar2,
    not the channel; unattributed further). The 8812AU uplink still needs
    that adapter on the bench. **CLOSED 2026-09-26**: the 8812AU as AP
    passes `thru` on ch6 with EDCCA cleared (`DEVOURER_AP_CCA_GATES=edcca`);
    with it on, this placement defers ~13x (witnessed) - see the Phase 5 row.
11. ~~**Soaks with the defaults.**~~ **CLOSED 2026-09-26** for the 8812CU:
    30 min, 5/5, up 0.00% every chunk, down <= 0.02%, 643k frames each way,
    replays 0 (one run). The 8812BU is item 12's run.
12. ~~**The 8812BU AP's resident memory grew 532 kB in 15 min**~~ **ANSWERED
    2026-09-26: warm-up, not a leak.** A 30-min 8812BU-AP soak with the
    defaults, RSS sampled every 30 s by an external sampler: 11516 -> 12008 kB
    inside the first minute, then 12012 kB flat for all 61 samples. The soak
    itself: 5/5, up 0.00% every chunk, down 0.00-0.06%, 643k frames each way,
    one association, replays 0; three TX-DMA watchdog reads failed and were
    skipped (the known Jaguar2 USB-read flakiness, guarded). One run.
13. ~~**The station's send ceiling is ~22.7 Mbit/s**~~ **ANSWERED
    2026-09-26: the MT7612U send path, not the CCMP.** Same AP (8812CU),
    same session, uplink ladder 20-50 Mbit/s, one run each: the MT7612U
    station tops out at 21.2-22.6 Mbit/s, an 8812BU station running the same
    `sta_client` at 28.4-29.8. Both encrypted every offered frame (187.5k);
    every loss is a send-queue drop. Why the MT7612U path stops there (USB
    submit depth, TXWI build, the chip) is not measured.
14. **Untested silicon** sharing today's fixes: the 8821C (USB) and the PCIe
    8821CE (Jaguar2 `MAC_TRX_ENABLE`), the 8814AU (Jaguar1 auto-LLT - not
    covered by the manual-LLT argument), and 2/4-bulk-OUT Jaguar3 parts (the
    endpoint map is the 3-bulk-OUT table). Needs hardware.
15. **Small, recorded, not chased:** Jaguar2's `rx.frame` `seq` is always 0;
    the MT7612U decoded ~half of an 8822E's 2.4 GHz beacons against the quirk
    entry that says no receiver does; the branch `xp/j3-general-info` (the
    byte-exact GENERAL_INFO port) - DECIDED 2026-09-26: pushed to the fork
    as a record (`fork/xp/j3-general-info`, 12dd4e7), not for merge.

16. **12 station MIC failures = 12 AP send failures** in the unarmed Phase 6
    control (8812BU station, 8812CU AP, 4x retry load): possibly frames aired
    truncated when a Jaguar3 send timed out, possibly coincidence. One run.
17. **Phase 6 remainder:** the 8822E and 8821C cells; a soak with a
    Realtek station. (Jaguar1 done: 8812AU, non-discriminating control.)
18. **Jaguar1 EDCCA at 2.4 GHz.** The 8812AU AP deferred ~13x on ch6 at its
    current placement with EDCCA on (the family's default). What trips it -
    a bench emitter, or a threshold the IGI-coupled adaptivity sets too low
    on 2.4 GHz - is not separated; compare against the kernel rtw88 driver's
    AP on the same placement, or move the adapter.
## The rule this work runs under

No gate closes without **at least two adversarial reviews** with findings
resolved — fixed, or rejected with a written reason. It has earned its keep
four times: it caught a Phase 0 measurement that could not test its own claim
(the verdict reversed), a CCMP API that would have silently broken Phase 2, a
replay-window reset an attacker could trigger with a captured msg4, and two
occasions where this branch's own write-up claimed more than its evidence
supported. Both of those overclaims are corrected in place rather than
quietly edited out, because the pattern — reaching a true conclusion by a
wrong route — is the thing worth noticing.
