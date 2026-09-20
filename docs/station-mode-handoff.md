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
| 1 — shared frame + crypto layer | **Substantially done, gate open.** See below. |
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
| station | RTL8812AU `0bda:8812` | `1-1` | `rtw_8812au`; **independent silicon**, which is the witness `docs/mt7612u-ap-mode.md` says it lacked |

```sh
sudo AP_SYSFS=7-1 STA_SYSFS=1-1 CH=6 tests/ap_onair_rtl_sta.sh both
```

`tests/ap_onair_rtl_sta.sh` encodes the three things that each cost a run:

1. The station must be **re-enumerated** (authorized toggle), not driver-bound.
   devourer's libusb claim leaves the interface on `usbfs`, and writing a
   driver's `bind` reports success without re-probing — no netdev appears.
2. `DEVOURER_BCN_TU=25`. `tests/mt7612u_ap_onair.sh` hardcodes 100 TU, which
   its own MediaTek station tolerated and this one does not.
3. Ping on `CTRL-EVENT-CONNECTED`, against a **truncated** supplicant log. This
   station drops on inactivity within seconds of a completed 4-way, so a fixed
   sleep straddles the timer; and a stale `CONNECTED` in an appended log
   matches instantly, producing the same wrong answer from the other side.

To free the MT7612U from the kernel: `echo 7-1:1.0 > /sys/bus/usb/drivers/mt76x2u/unbind`.

## What the on-air runs actually showed

```
  data plane: encrypted frames received=98, MIC failures=0,
              replays rejected=1, frames sent=27
  5 packets transmitted, 5 received, 0% packet loss
```

Beacon scannable, open auth+assoc complete, WPA2 4-way complete with a verified
MIC, and ~250 encrypted frames decrypted across two runs with **zero MIC
failures**. 2.4 GHz only.

## What to distrust

- **`tests/mt7612u_ap_onair.sh` 14/14 on both bands has NOT been re-run.** That
  is the formal acceptance bar and it is outstanding. Everything above is
  manual cells on one band against a station that harness cannot drive.
- **The link is unstable on this rig.** Roughly half the runs never carry a
  packet — the station drops on inactivity after the handshake. The ledger is
  what distinguishes those (`received=0`) from a crypto failure; without it
  they read identically, and an earlier session spent a long time believing
  the refactor had broken something.
- **RTT is poor and unexplained** — 524 ms mean, 1729 ms max, near-field.
- **Nothing is soaked.** Longest run under a minute.
- **The sequence-number causality is inference.** The data plane went from
  100% loss to 0% when data-frame sequence numbers landed, and the QoS AAD fix
  had already shipped before the failing run — so by elimination it was the
  sequence number. That is an ordered pair of runs, not an A/B.
- **5 GHz `iw connect` fails with -22** under a no-IR regulatory domain even
  though the beacon scans fine. Not investigated.
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
