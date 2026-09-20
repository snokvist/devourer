#!/usr/bin/env python3
"""Inject unicast 802.11 data frames at a MAC from a monitor interface.

Companion to tests/mt7612u_sta_identity.sh and `mt7612uprobe sta`.

That gate counts what a managed station RECEIVES. Beacons alone only exercise
broadcast reception, and the question it exists to answer is whether the BSSID
registers gate UNICAST delivery to the station's own address. hostapd sends an
unassociated station no unicast at all, so the first run of the gate read
to_us=0 in every arm and could not have answered it. This makes the traffic.

The monitor vif lives on the AP's own phy, so the injection rides the AP's
radio and lands on the AP's channel without needing a third adapter.

  sta_unicast_inject.py <mon-if> <dst-mac> <bssid> [seconds] [pps]

Note what this does NOT do: mac80211 marks injected frames no-ack by default,
so these do not solicit an acknowledgement and cannot be used to measure one.
Use `mt7612uprobe staack` for that - it makes the AP answer through its normal
transmit path and counts the retried copies.
"""
import socket
import struct
import sys
import time

# version 0, pad 0, length 8, present bitmap 0 - no fields, just the header.
RADIOTAP = struct.pack('<BBHI', 0, 0, 8, 0)


def mac(s):
    parts = s.split(':')
    if len(parts) != 6:
        raise ValueError('not a MAC address: %r' % s)
    return bytes(int(x, 16) for x in parts)


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    mon = argv[1]
    dst = mac(argv[2])
    bssid = mac(argv[3])
    secs = float(argv[4]) if len(argv) > 4 else 120.0
    pps = float(argv[5]) if len(argv) > 5 else 300.0

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    sock.bind((mon, 0))

    # Data frame, FromDS: addr1 = the station, addr2 = addr3 = the BSSID.
    # That is the shape an AP's downlink traffic has, which is what a station
    # would be filtering for.
    body = b'STA-UNICAST-PROBE' * 4
    gap = 1.0 / pps if pps > 0 else 0.0
    end = time.time() + secs
    sent = 0
    seq = 0
    while time.time() < end:
        hdr = (struct.pack('<BBH', 0x08, 0x02, 0) + dst + bssid + bssid +
               struct.pack('<H', (seq & 0x0fff) << 4))
        try:
            sock.send(RADIOTAP + hdr + body)
            sent += 1
        except OSError:
            # A busy or momentarily-down monitor vif is not fatal here; the
            # gate cares about the arms being comparable, not about any single
            # frame landing.
            time.sleep(0.01)
        seq += 1
        if gap:
            time.sleep(gap)
    print('injected %d unicast frames at %s' % (sent, argv[2]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
