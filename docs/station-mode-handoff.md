# Station mode — where the work stands, and how to pick it up

Companion to `docs/station-mode-scope.md` (the technical scope),
`docs/station-mode-plan.md` (phases, gates, the review ledger) and
`docs/station-mode-phase0.md` (the Phase 0 measurement). This file is the
session-continuity note: what exists, where it lives, how to run it, and what
the next person should distrust.

## Location

Worktree `.claude/worktrees/mt7612u-station-scope`, branch
`worktree-mt7612u-station-scope`, based on `origin/master` at `14a4881`.
Nothing is pushed and no PR exists.

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
| 2 — the `IRadio` seam | Not started. `SetStationIdentity` / `ClearStationIdentity`, MT7612U implementation, caps flag. |
| 3 — pure station logic | Not started. BSS table, association state machine, 4-way supplicant. |
| 4–6 | Not started. |

## What exists now

Pure, backend-neutral, under `src/sta/` — no device, no environment, no clock,
no threads, no sockets:

- `CryptoOps.h` — the crypto vtable, so `libdevourer` gains no OpenSSL.
- `Ccmp.h` — 802.11 CCMP framing: AAD (incl. QoS TID and 4-address A4),
  nonce, CCMP header, PN, encrypt/decrypt, per-TID replay window.
- `Dot11.h` — management frames: header builder, IE builders, a bounds-checked
  IE walker, RSN build **and parse**, beacon/auth/assoc parsers,
  station-side request builders, sequence counter, data-frame headers.

Tested headless by `ctest`: `ccmp_framing`, `dot11_frames`,
`ccmp_software_roundtrip`. **71/71 green.** Both selftests were verified
capable of failing by injecting the defect they exist to catch.

Both AP harnesses (`tests/ap_responder.cpp`, `tests/ap_wpa2.cpp`) are switched
onto these modules. That is the load-bearing property: a station module only a
station used would be a station module with AP-shaped holes.

MT7612U-specific, in the same branch: `MT_TX_STAT_FIFO` declared and
`MT_TXOPT_TXS` added so the chip reports its own retry count
(`bringup txs`, `docs/mt7612u-tx-retry.md`), plus `bringup ucast` (`gate_ucast`)
for the Phase 0 measurement.

## The rig, and how to reproduce the on-air cells

| role | part | sysfs | notes |
|---|---|---|---|
| AP | MT7612U `0e8d:7612` | `7-1` | USB **2.0** link; firmware from `/lib/firmware/mediatek` |
| station | RTL8812AU `0bda:8812` | `1-1` | the **in-tree rtw88** driver: module `rtw88_8812au`, sysfs driver name `rtw_8812au` (both spellings are in this tree; they name different things). **Independent silicon**, which is the witness `docs/mt7612u-ap-mode.md` says it lacked |

```sh
sudo AP_SYSFS=7-1 STA_SYSFS=1-1 CH=6  FW_DIR=/lib/firmware/mediatek tests/mt7612u_ap_onair.sh all
sudo AP_SYSFS=7-1 STA_SYSFS=1-1 CH=36 FW_DIR=/lib/firmware/mediatek tests/mt7612u_ap_onair.sh all
```

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

## What the on-air runs actually showed

`tests/mt7612u_ap_onair.sh` reads **14/14 twice on 2.4 GHz (ch6) and twice on
5 GHz (ch36)**, at the default `BCN_TU=100`. Beacon armed and scannable, open
auth+assoc, hardware auto-ACK, an ARP/ICMP data plane, the WPA2 4-way with a
verified MIC, an encrypted data plane, and the full beacon lifecycle
(arm → stop → re-arm) on both bands. RTT 0.7–2.4 ms in every cell.

The adversarial counterpart, in the same breath, because this tree's rules
require it:

- **Power save is forced off, and this AP cannot serve a power-saving station
  at all.** None of the three beacon builders appends a TIM element. Linux
  defaults to `power_save on`, so 14/14 does *not* certify this AP against a
  default-configured Linux client.
- The 5 GHz open cell's data-plane check **failed once in three runs** on a
  single lost ping — 6 packets at 0% loss on a channel with several strong
  neighbours. Brittle by construction; recorded, not loosened.
- n=2 per band, one AP unit, one station unit, near field, nothing soaked
  (longest run about two minutes).

## What to distrust

- **Power save is the single biggest caveat on the 14/14.** It is forced off,
  and this AP has no TIM element in any beacon and buffers nothing, so it
  cannot serve a power-saving station — which is what Linux defaults to. The
  score is real and its scope is narrower than it looks.
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
- **The ledger lies on Realtek.** `Packet::Data` carries a trailing FCS on
  every Realtek generation and not on MT7612U, and `on_rx` does not trim it,
  so a Realtek AP would report a length bug as MIC failures. Fix the trim
  before trusting it outside MediaTek — this matters at Phase 6.

## Open, carried forward

1. **The ~20 ms No-Ack unicast cost.** `docs/mt7612u-tx-retry.md`: the MAC
   completes these on the first attempt with 0 retries and 100% success, and
   they still cost ~20 ms. So it is **not** the retry ladder — this branch
   refuted its own earlier hypothesis. Arms for WCID, QSEL and A-MPDU are in
   `bringup txs` and **have not been run**. Matters for injection, not for a
   station.
2. **`CcmpReplay` is a strict counter, not a bitmap.** No reorder tolerance; a
   late retransmission across a hole is dropped. Safe while the AP declines
   ADDBA, wrong as a general rule.
3. **The KAT vectors pin the cipher plumbing, not the framing rules.** Stated
   plainly in `tests/ccmp_gen_vectors.py`. The IEEE 802.11-2016 Annex J CCMP
   vector is a drop-in improvement and the selftest is shaped for it.
4. **`SetStationIdentity` is provisional.** It ships with one implementation;
   `docs/station-mode-scope.md` argues the position honestly and Phase 2's gate
   should weigh landing the Realtek arm alongside it.

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
