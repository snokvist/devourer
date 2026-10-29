# Master prompt — local hardware session for p2plink (PR #4)

Paste everything below the line into the local Claude Code session started in a
checkout of `snokvist/devourer` on branch `ccr-2c660d65-jhhngr`, with the
adapters plugged in.

---

You are continuing work that a cloud session started and that you now own on
hardware. Repo: this checkout of snokvist/devourer, branch
`ccr-2c660d65-jhhngr`, open as draft PR #4 against master
(https://github.com/snokvist/devourer/pull/4). Everything you touch lands as
further commits on this branch; never push to master, never force-push, and
never commit anything carrying an AI model name.

## What exists

`p2plink/` is a new self-contained subproject: a range-first 1:1 video + IP
link on the devourer userspace driver. Drone = one RTL8812EU reading the
waybeam encoder frame ring plus a tun device. Ground = one or more adapters
(8812EU and/or MT7612U) as receive ears, one of them the ACK target and
optionally the uplink. Read, in this order, before touching anything:

1. `p2plink/docs/DESIGN.md` — the spec as built (ladder, sensors, state
   machines, scheduler, merge, implementation notes).
2. `p2plink/docs/HANDOVER.md` — status, the six unverified assumptions, bench
   setup, bring-up order, cells C1–C9 with pass criteria.
3. `p2plink/README.md`, then the header comments in `p2plink/include/p2p/*.h`.
4. The repo root `CLAUDE.md` hardware-testing section: rtw88/mt76 auto-probe
   (temp-blacklist, `modprobe -r` does not survive re-enumeration), real VBUS
   cold vs `authorized` toggles, qualify the ground station before believing a
   delivery number, a single probe is worth ±3 points.

The pure cores (wire, FEC, merge, link controller, scheduler, frame ring) are
headless-tested: `ctest --test-dir build-p2p -L p2p` must stay 6/6 green and
warning-free after every change. The devourer glue (`src/radio.cpp`) and the
three programs (`p2p_tx`, `p2p_rx`, `p2p_feed`) have NEVER run against an
adapter. Expect bring-up bugs there; that is the first thing you are for.

## Your job, in order

1. **Review PR #4** as an adversarial reader of `radio.cpp`, `p2p_tx.cpp`,
   `p2p_rx.cpp` against the devourer contracts they rely on (`src/IRadio.h`
   callback/deadlock rules, `DeviceConfig.h` field docs, `examples/duplex` and
   `examples/sense` patterns). Fix what you find; one commit per defect class.
2. **Build and first light** per HANDOVER §3: ground first (`p2p_rx` prints
   `ack_target_mac`), then the drone with `--fixed-rung 2` and `p2p_feed
   --synthetic`. Working = `uniq` rising, `diversity ≈ uniq×(ears−1)` near
   field, `fec_fail_permille 0`, `ack8 == 8`, `p2p.da` Ucast.
3. **Run cells C1–C9 in order**, exactly as written, one at a time. Each cell
   names the design decision that flips if it fails — when one fails, stop,
   write the finding, propose the flip, and ask before redesigning. Do not skip
   ahead to a later cell on a failed earlier one unless it is independent.
4. **Record results** in a new `## Results` section at the end of
   `p2plink/docs/HANDOVER.md`: per cell, date, units (serial/bus path), channel,
   attenuation/distance, the commands, the numbers, pass/fail, and the log
   path. Every favourable number beside its adversarial counterpart (devourer
   house rule). A result that cannot be used must not report success.
5. **Comment on PR #4** once per milestone (first light, C1–C4 done, C5–C9
   done) with a short table of pass/fail, not a narrative.

## Rules of evidence (do not rediscover these)

- The ACK decision on Realtek is addr1 only; radiotap NOACK is MT7612U-only.
  Retry limit and ACK timeout are construction-time (3 / 128 µs here).
- The MT7612U auto-ACKs its factory MAC unarmed; never `SetStationIdentity`
  on an ear (it installs the managed filter and the ear goes deaf to
  other-MAC unicast and to retried copies). Never send unicast FROM an
  MT7612U (75 fps cliff, ~20 ms per frame).
- The MT7612U does not bring up on a saturated channel: ground before drone,
  and if the ground reboots mid-run, pause the drone.
- `wide_tx_igi_floor` (default 0x35) is armed by the drone's first 40 MHz
  frame and stays while tuned to 40 MHz; its RX-sensitivity cost is estimated
  (~21 dB) and unmeasured — C4 measures it. The edge is per unit.
- The 8812EU stalls a synchronous send for the full 20 ms bulk-OUT timeout now
  and then; `no_cancel_multipkt` is hard-wired on so this cannot become a TXDMA
  wedge. If `tx_submitted` ever stops rising while the chip is alive, that is a
  finding, not a flake.
- A missing `tx.report` is UNKNOWN, not a failure. CCX coverage falls above
  ~1.3k reports/s.
- Two adapters of the same part number are not a replicate. Keep a known-good
  control adapter and re-check it each session.
- Near field (~30 cm) saturates the front end: EVM, not SNR, is the tell;
  attenuate or back off power before judging delivery.
- Nothing in this project enforces regulatory limits or does DFS detection.
  You own channel choice and power.

## Boundaries

- Fix p2plink freely. Do not modify devourer sources outside `p2plink/`
  without writing down why in the commit and the PR; prefer a p2plink-side
  workaround and an upstream note.
- Kill bench processes only from a script file with SIGTERM
  (`pkill -TERM -f 'build-p2p/p2p_'` typed interactively can match your own
  shell).
- Ask before anything irreversible or outward-facing: force-pushes, deleting
  branches, posting outside PR #4, running at full power on a shared channel.
- When a cell result contradicts DESIGN.md, the doc changes in the same commit
  as the code; the spec is never left describing behaviour the tree no longer
  has.

Start by reading the four items above, then report in one short message what
you will run first and which adapters you see (`lsusb`, bus paths, kernel
drivers bound).
