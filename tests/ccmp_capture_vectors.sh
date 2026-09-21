#!/bin/sh
# Regenerate tests/ccmp_kernel_vectors.h from a mac80211_hwsim rig.
#
# WHY THIS EXISTS. tests/ccmp_vectors.h pins the CIPHER against a third
# implementation, but its generator and src/sta/Ccmp.h share one author's
# reading of the 802.11 framing rules - which is how a CCM nonce with a zero
# Flags octet stayed green through the whole vector suite while being wrong
# for every TID except 0. This script does not read the standard at all. It
# makes the LINUX KERNEL encrypt frames and copies the bytes.
#
# NO HARDWARE. Two virtual radios, so this does not touch the bench. It does
# need root (module load, netns, a monitor capture) and hostapd,
# wpa_supplicant and tcpdump on PATH.
#
# THE TID IS THE WHOLE POINT. hostapd runs with wmm_enabled=1, so the station
# sends QoS data frames; `ping -Q` does NOT give exact TID control (setting
# IP_TOS sets sk_priority through ip_tos2prio[] before cfg80211_classify8021d
# ever sees the DSCP), so the traffic generator sets SO_PRIORITY to 256+tid,
# which that function special-cases as "user priority tid, verbatim". That is
# what reaches all eight.
#
# The extracted frames' key material is a throwaway lab PSK on virtual radios
# and protects nothing.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$(mktemp -d /tmp/ccmpvec.XXXXXX)}
NS=ccmpvec_sta
PSK=ccmpvectors123
SSID=ccmpvec

[ "$(id -u)" = 0 ] || { echo "this needs root"; exit 1; }

cleanup() {
    # BY PID. `pkill -x tcpdump` killed every unrelated capture on the host,
    # which on a machine that is also a radio bench is somebody else's run.
    [ -n "${CAP_PID:-}" ] && kill "$CAP_PID" 2>/dev/null || true
    pkill -f "hostapd .*$WORK" 2>/dev/null || true
    ip netns exec "$NS" pkill -f wpa_supplicant 2>/dev/null || true
    ip netns del "$NS" 2>/dev/null || true
    rmmod mac80211_hwsim 2>/dev/null || true
}
trap cleanup EXIT

echo "--- two virtual radios"
rmmod mac80211_hwsim 2>/dev/null || true
modprobe mac80211_hwsim radios=2
sleep 2
# The two phys hwsim just created are the two highest-numbered ones.
PHYS=$(ls /sys/class/ieee80211 | sed 's/phy//' | sort -n | tail -2)
AP_PHY=phy$(echo "$PHYS" | head -1)
STA_PHY=phy$(echo "$PHYS" | tail -1)
if_for_phy() {
    for l in /sys/class/net/*/phy80211; do
        [ -e "$l" ] || continue
        if [ "$(basename "$(readlink -f "$l")")" = "$1" ]; then
            basename "$(dirname "$l")"
            return
        fi
    done
}
AP_IF=$(if_for_phy "$AP_PHY")
STA_IF=$(if_for_phy "$STA_PHY")
[ -n "$AP_IF" ] && [ -n "$STA_IF" ] || { echo "no hwsim interfaces"; exit 1; }
echo "    AP  $AP_PHY/$AP_IF   STA $STA_PHY/$STA_IF"
nmcli dev set "$AP_IF" managed no 2>/dev/null || true
nmcli dev set "$STA_IF" managed no 2>/dev/null || true

echo "--- the station gets its own namespace"
ip netns add "$NS"
# A cfg80211 interface cannot be moved with `ip link set netns`; the whole phy
# moves or nothing does.
iw phy "$STA_PHY" set netns name "$NS"
ip netns exec "$NS" ip link set "$STA_IF" up
ip netns exec "$NS" ip addr add 10.77.0.2/24 dev "$STA_IF"

cat > "$WORK/hostapd.conf" <<EOF
interface=$AP_IF
driver=nl80211
ssid=$SSID
hw_mode=g
channel=1
wmm_enabled=1
ieee80211n=1
auth_algs=1
wpa=2
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
wpa_passphrase=$PSK
EOF
cat > "$WORK/wpa.conf" <<EOF
network={
	ssid="$SSID"
	psk="$PSK"
	key_mgmt=WPA-PSK
	proto=RSN
	pairwise=CCMP
	group=CCMP
}
EOF

echo "--- capture, AP, station"
ip link set hwsim0 up
ip link set "$AP_IF" up
setsid tcpdump -i hwsim0 -s 0 -w "$WORK/cap.pcap" -U >"$WORK/tcpdump.log" 2>&1 &
CAP_PID=$!
sleep 2
setsid hostapd -dd -K -t "$WORK/hostapd.conf" >"$WORK/hostapd.log" 2>&1 &
sleep 4
grep -q AP-ENABLED "$WORK/hostapd.log" || { echo "hostapd did not come up"; exit 1; }
ip addr add 10.77.0.1/24 dev "$AP_IF"
# -K is what dumps the derived keys; without it there is no TK to extract.
ip netns exec "$NS" setsid wpa_supplicant -i "$STA_IF" -c "$WORK/wpa.conf" \
    -dd -K -t -f "$WORK/wpa.log" >/dev/null 2>&1 &
i=0
while [ $i -lt 30 ]; do
    grep -q CTRL-EVENT-CONNECTED "$WORK/wpa.log" 2>/dev/null && break
    i=$((i + 1)); sleep 1
done
grep -q CTRL-EVENT-CONNECTED "$WORK/wpa.log" || { echo "no association"; exit 1; }
TK=$(grep -m1 'WPA: TK - hexdump' "$WORK/wpa.log" | sed 's/.*): //; s/ //g')
[ ${#TK} = 32 ] || { echo "could not read the TK"; exit 1; }
echo "    associated, TK is 16 bytes"

echo "--- one datagram per user priority, in each direction"
ip netns exec "$NS" ping -c 1 -W 3 10.77.0.1 >/dev/null 2>&1 || true
ip netns exec "$NS" python3 "$HERE/ccmp_tid_send.py" 10.77.0.1
python3 "$HERE/ccmp_tid_send.py" 10.77.0.2
sleep 2
kill "$CAP_PID" 2>/dev/null || true
sleep 1

python3 "$HERE/ccmp_extract_vectors.py" "$WORK/cap.pcap" "$TK" \
    "$HERE/ccmp_kernel_vectors.h"

# VERIFY ITS OWN OUTPUT. The extractor refuses a short vector set and an FCS
# flag, but a TK read from the wrong hexdump line would still produce a
# plausible-looking header that only fails when somebody else builds. Running
# the test here is what makes a bad regeneration this script's failure.
BUILD=${BUILD:-$HERE/../build}
if [ -f "$BUILD/CMakeCache.txt" ]; then
    echo "--- rebuilding and running CcmpSelftest against the new vectors"
    cmake --build "$BUILD" --target CcmpSelftest >/dev/null 2>&1 \
        || { echo "FAIL: CcmpSelftest does not build"; exit 1; }
    "$BUILD/CcmpSelftest" || { echo "FAIL: the new vectors do not verify"; exit 1; }
else
    echo "--- no build tree at $BUILD; build and run CcmpSelftest yourself"
fi
echo "--- done"
