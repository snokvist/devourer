
## Channel busy airtime (`GetChannelBusy`)

`Mt7612uRadio::GetChannelBusy()` implements the vendor-neutral
`IRadio::GetChannelBusy` from the MAC channel timers, via `mt7612u_ch_time()`.
Register pair, arming, read-and-clear and refusal semantics are documented at
that function's declaration (`include/mt7612u/mt7612u.h`) — the one place they
can be kept true.

The subtree-specific facts: the arming configuration is mt76's
`mt76x02_mac_cc_reset()` exactly, which counts TX as busy, so a transmitting
radio measures its own airtime and must read inside a quiet window. Timers are
armed on both the RX and the transmit-only path and re-armed on every live
retune. `ChannelBusy::energy_pct` is left invalid here because the only
candidate counter, `MT_RX_STAT_1`'s false-CCA field, is owned by
`mt7612u_phy_tick()`'s AGC loop.

**Not validated on hardware** — no MT7612U was available. `busy_airtime_ok` is
true, `busy_airtime_measured` is false. Two things to measure when one is on
the bench: that busy/idle track real occupancy beyond repetition noise, and
that polling at dwell cadence does not disturb `phy_tick`'s gain tracking. For
the latter, reuse the tick's own controlled benchmark — with and without the
poller, against a steady peer — reading the full figures, control arm included,
from the `mt7612u_phy_tick` doc comment rather than a copy of its headline
number.

## Radiotap NOACK disables the hardware retry

`radiotap.cpp` maps TX_FLAGS NOACK to `rate->no_ack`, and `tx.cpp` then leaves
the TXWI `ACK_CTL_REQ` clear: the MAC marks the frame done on the first
attempt and its 15-deep retry (`MT_TX_RETRY_CFG`, `docs/mt7612u-tx-retry.md`)
never runs. `build_stream_radiotap(mode)` always sets NOACK - right for a
broadcast stream, wrong for unicast to a peer that ACKs. A station must use
`build_stream_radiotap(mode, /*no_ack=*/false)` for unicast and keep NOACK
for group-addressed frames (`tests/sta_client.cpp`, `DEVOURER_STA_ACK`).
Measured at ch36, 6M: uplink loss ~1% -> 0.00% (5386/5386), retries visible
on air. The trap it set: "a witness sees zero station retries" reads like
proof the peer ACKed everything, and it proves nothing while NOACK is set.
