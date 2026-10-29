# p2plink — a range-first 1:1 video + IP link on devourer

p2plink is a point-to-point WiFi link for one drone and one ground station,
built on the devourer userspace driver in the parent tree: H.265 video from a
waybeam-format encoder frame ring plus an IP tunnel, both over raw 802.11
injection on 5 GHz, with hardware ACK used as a *sensor and an addressing
policy* rather than as the reliability mechanism (FEC is). Range before
throughput: no control action may on its own cause data loss (no blind width or
channel moves, no retry storms, no encoder overshoot).
It is **research code, not flight-proven**: the pure core is unit-tested, the
radio glue and both apps have never run against hardware — see
`docs/HANDOVER.md` before plugging anything in; `docs/DESIGN.md` has the rules.

## The mechanism, in five bullets

- **DA = ACK policy.** On every Realtek die a frame is ACKed and retried iff
  its addr1 is unicast; a group DA is never ACKed nor retried. Video is ALWAYS
  group-addressed; IP and control flip between the ACK target's unicast MAC
  and the group DA under `DaMachine`. The retry limit is construction-time (3)
  and never moves; an absent peer at retry 12 cost 12.3 airings per frame.
- **A probe's ACK is the primary uplink sensor.** A ≤32 B unicast probe goes
  to the ACK target every 100 ms in every state at the current rung; its
  `tx.report` arrives ~2 ms later. `ack8` = oks over the last 8 *reported*
  probes (a missing report is unknown, not a failure). The 10 Hz LinkReport
  is secondary: on the reference link 89 % of returns landed while the craft
  was transmitting and deaf; the ACK arrives SIFS after our own frame.
- **Ladder = {20|40 MHz, MCS, FEC k/n, duty}; 20/40 only.** The ground parks
  at 40 MHz; the drone picks 20 or 40 per frame in radiotap (wide-RX penalty
  measured 0 dB, ≤1 dB — on one other TX/RX pair at 80/40). No 5/10 MHz (the
  MediaTek ear has no narrowband decode), no 2.4 GHz, no in-flight channel
  changes. Frame body fixed at 1400 B.
- **Budget leads, PHY follows.** A demote re-targets the encoder bitrate and
  FEC 500 ms before the MCS steps (a demote without the re-target was ~98 %
  unrecoverable on the reference link); a promote does the opposite. With no
  fresh report for 500 ms the controller descends two rungs per 500 ms to the
  floor (~2.5 s from the top) unless the probe ACKs still land (`ack8` ≥ 6),
  in which case it HOLDs and never promotes.
- **N-ear merge.** Ground adapters receive promiscuously; the first copy of
  each (stream, block, symbol) wins, later copies count as `diversity`, a
  200 ms-silent ear is excluded. Measured elsewhere, two ears: 86 → 24 ‰ loss
  on a walk; 15 % independent loss each → 2.1 % joint, but only 4.9 → 2.6 %
  when the loss is correlated at 65 % airtime.

## Roles and slots

Drone: one RTL8812EU (`p2p_tx`). Ground (`p2p_rx`): 1..N adapters in three
slots — `ack_target` (whose permanent MAC the probe addresses: an 8812EU via
`SetAckResponder`, or an MT7612U, which auto-ACKs its factory MAC), `uplink`
(reports + IP: 8812EU preferred; an MT7612U may, broadcast only — its unicast
TX is a 75 fps / 0.83 Mbit/s cliff against 3037 fps broadcast), `ears` (monitor
only). One MT7612U can fill all three. Probe DA == ack_target MAC, ∉ other ears.

## Build

From the devourer checkout root (this tree is a subdirectory of it):

```sh
cmake -S p2plink -B build-p2p -DCMAKE_BUILD_TYPE=Release [-DP2P_WITH_MT7612U=ON]
cmake --build build-p2p -j
ctest --test-dir build-p2p -L p2p  # wire fec merge link_ctl budget frame_shm
```

`P2P_WITH_MT7612U` (default ON) needs `mt7662_rom_patch.bin` + `mt7662.bin` at
run time (`--mt7612u-fw DIR`, HANDOVER §2). Only the 8822C/8822E groups are built.

## Run — ground first, then drone

```sh
# ground: 8812EU as ACK target + uplink (ear 0), two MT7612U ears
sudo ./build-p2p/p2p_rx --adapter 0bda:a81a --adapter 0e8d:7612@1-1.1 \
    --adapter 0e8d:7612@1-1.2 --ack-target 0 --uplink 0 \
    --channel 100 --bw 40 --net-id 7 --out-udp 127.0.0.1:5600 \
    --tun p2pg --tun-addr 10.99.0.2/30 --mt7612u-fw ./firmware
# it prints {"ev":"p2p.ready",...,"ack_target_mac":"XX:..."} — copy that MAC
# drone
sudo ./build-p2p/p2p_tx --adapter 0bda:a81a --channel 100 --bw 40 --net-id 7 \
    --peer-mac XX:XX:XX:XX:XX:XX --ring /venc.video0 \
    --tun p2pd --tun-addr 10.99.0.1/30 \
    --bitrate-cmd 'curl -s http://127.0.0.1/api/v1/set?video0.bitrate=%u' \
    --start-rung 2
```

Ground first: MT7612U bring-up does not reliably complete on a saturated
channel (its MCU times out). `p2p_feed --ring NAME --synthetic --fps 30
--kbps 4000` stands in for the encoder on a bench; full CLI in HANDOVER §3.

## Where stats land

Every 100 ms each app publishes a `StatsSnapshot` (`include/p2p/report.h`) to
a POSIX shm object (`/p2plink.tx` / `/p2plink.rx`, `--stats-shm` renames;
`p2p::StatsPublisher::read` reads it from any process) and as one JSON line on
stdout, `{"ev":"p2p.stats",...}`. Other events: `p2p.ready`, `p2p.rung`, `p2p.da`,
`p2p.tune`, `p2p.exit`; `grep -F '"ev":"p2p.rung"'` needs no JSON parser.
`--no-ndjson` silences stdout; devourer diagnostics stay on stderr.

## Deliberately out of scope (v1)

Encryption, channel migration, cache/repair side-links, multi-receiver authority,
calibration, narrowband, 2.4 GHz, A-MPDU, frame size as a rung, regulatory enforcement.

## Layout

- `include/p2p/wire.h` — 802.11 encapsulation, 18 B P2pHeader, group/link MACs
- `include/p2p/report.h` — LinkReport (32 B), Probe (8 B), StatsSnapshot
- `include/p2p/fec.h` — systematic RS erasure code over GF(256), zfec-compatible
- `include/p2p/link_ctl.h` — ladder, LinkController (TRACK/BLIND/HOLD/FLOOR), DaMachine
- `include/p2p/budget.h` — airtime model + strict-priority TX scheduler
- `include/p2p/rx_merge.h` — N-ear BlockMerger (first copy wins) + SeqDedup
- `include/p2p/frame_shm.h` — waybeam venc frame-ring consumer/producer
- `include/p2p/stats_pub.h` — shm + NDJSON StatsSnapshot publisher
- `include/p2p/tun.h` — Linux TUN device + DSCP classifier
- `src/` — `fec.cpp`; io layer `radio.cpp`, `tun.cpp`, `frame_shm.cpp`,
  `stats_pub.cpp`; apps `p2p_tx.cpp`, `p2p_rx.cpp`, `p2p_feed.cpp`
- `tests/` — one `_test.cpp` per core unit (ctest); `docs/` — DESIGN.md, HANDOVER.md

## License

GPL-2.0-or-later, like devourer. The frame-ring layout in `frame_shm.h` is a
byte-for-byte mirror of waybeam-link's `frame_shm_format.h` (GPL-2.0-or-later)
so the same encoder binary feeds this link unchanged.
