#!/usr/bin/env bash
# mt7612u_ap_onair.sh — the whole MT7612U AP claim, end to end, on hardware.
#
# Every number in docs/mt7612u-ap-mode.md's "Verified through IRadio" table
# comes from this script. It exists because those numbers were hand-run first,
# and three of the readings were wrong in ways that a script would not have
# repeated:
#
#   - "the beacon was gone after the process exited" measured nothing. Both AP
#     harnesses used to end in _exit(0), so StopBeacon never ran; the beacon
#     was still airing and one scan happened to miss it.
#   - `iw scan` without `flush` reports a stopped beacon as present for ~30 s
#     out of its BSS cache.
#   - a bring-up that fails leaves no beacon either, so "no beacon" read as a
#     pass when the AP had in fact never started. Every phase here checks the
#     AP came up BEFORE it believes an absence.
#
# Three cells, each with its own witness:
#
#   open   ap_responder     beacon -> scan, associate, ARP/ICMP ping
#   wpa2   ap_wpa2          beacon -> scan, 4-way handshake, encrypted ping
#   stop   beacon_stop_check armed -> stopped -> re-armed, by scan
#
# Bench: the AP is an MT7612U that devourer claims. The STATION is any adapter
# the host has a working kernel driver for, named by sysfs id - it is driven
# only through iw / wpa_supplicant / ping and this script never assumes its
# chip. The original bench used a second MT7612U on mt76x2u, which made the
# witness same-silicon on both ends; an independent-generation station (an
# RTL8812AU on rtw88_8812au) is the stronger reading of the same cells, and is
# what docs/mt7612u-ap-mode.md admitted it lacked.
#
#   sudo tests/mt7612u_ap_onair.sh
#   sudo AP_SYSFS=5-1 STA_SYSFS=2-1 CH=36 tests/mt7612u_ap_onair.sh open
#   sudo AP_SYSFS=7-1 STA_SYSFS=1-1 CH=6 BCN_TU=25 tests/mt7612u_ap_onair.sh all
#
# BCN_TU sets the beacon interval. It is a knob because docs/ap-mode.md claims
# "a dense beacon interval (DEVOURER_BCN_TU=25) is needed throughout" - but on
# this bench that claim does not reproduce, and the harness is the evidence:
#
#   - the `stop` cell's binary (mt7612u_beacon_stop_check.cpp:74,123,191)
#     HARDCODES 100 TU and ignores DEVOURER_BCN_TU entirely, and its three
#     scan checks passed on both bands in every run;
#   - a full `all` run at the default BCN_TU=100 scored 14/14 on ch6.
#
# So 25 was not needed here. An earlier note in this tree said this station
# "misses a 100 TU beacon entirely"; that is not what was measured and the
# claim is withdrawn. If a future station really does need a denser beacon,
# lower this - but expect the `stop` cell to keep airing 100 TU regardless
# until that binary learns the variable.
#
# Env: AP_SYSFS, STA_SYSFS, CH, BCN_TU, PSK, FW_DIR, SECS, AP_VBUS (hubloc:port
# for a real VBUS cold cycle via uhubctl; hub ports only). BENCH_SECS and
# BENCH_PAYLOAD configure the CCMP CPU/throughput cell.
# Cells: open|wpa2|stop|bench|all (all remains the acceptance cells only).

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CELLS="${1:-all}"

AP_SYSFS="${AP_SYSFS:-5-1}"
STA_SYSFS="${STA_SYSFS:-2-1}"
CH="${CH:-36}"
# FREQ is load-bearing in three places now - the scan, seen()'s match key and
# the pinned `iw connect` - so a wrong value turns every "is it airing" check
# into a silent "not scannable" and every "is it gone" check into a PASS. The
# old expression returned 2477 for channel 14 (it is 2484) and nonsense for
# 15..33, which no longer merely restricted a scan.
case "$CH" in
  ''|*[!0-9]*) echo "CH must be a channel number"; exit 2 ;;
esac
if   [ "$CH" -ge 1 ]  && [ "$CH" -le 13 ]; then FREQ=$(( 2407 + CH * 5 ))
elif [ "$CH" = 14 ];                       then FREQ=2484
elif [ "$CH" -ge 34 ] && [ "$CH" -le 196 ]; then FREQ=$(( 5000 + CH * 5 ))
else echo "CH=$CH is not a 2.4 or 5 GHz channel this harness can map"; exit 2
fi
PSK="${PSK:-devourer123}"
BCN_TU="${BCN_TU:-100}"
# SECS bounds the AP binary. seen() can now spend ~15 s and `iw connect` up to
# 30 s, so 40 let the AP exit underneath its own data-plane check.
SECS="${SECS:-70}"

# The identities the AP sources hardcode. Duplicated here because the harness
# drives the binaries as black boxes; kept in one place so the duplication is
# visible rather than scattered across six call sites.
#   ap_responder.cpp / ap_wpa2.cpp     -> kSsid, kBssid
#   mt7612u_beacon_stop_check.cpp      -> its own pair
AP_SSID="${AP_SSID:-devourerAP}"
AP_BSSID="${AP_BSSID:-02:42:75:05:d6:00}"
STOP_SSID="${STOP_SSID:-mtStopCheck}"
STOP_BSSID="${STOP_BSSID:-02:4d:54:53:54:50}"
BENCH_SECS="${BENCH_SECS:-15}"
BENCH_PAYLOAD="${BENCH_PAYLOAD:-1400}"
FW_DIR="${FW_DIR:-}"
APIP=192.168.99.1
STAIP=192.168.99.2
OUT="${OUT:-/tmp/mt7612u-ap-onair}"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
mkdir -p "$OUT"
pass=0; fail=0
say()  { printf '%s\n' "$*"; }
ok()   { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad()  { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

# PIDs this script started, so cleanup kills those and nothing else. `pkill -x
# wpa_supplicant` would drop every wireless client on the host, and a name kill
# would reach a concurrent run of this same test.
KIDS=""
reap() {
  local pid
  for pid in $KIDS; do kill "$pid" 2>/dev/null; done
  KIDS=""
}

cleanup() {
  reap
  [ -n "${STA_IF:-}" ] && { ip addr flush dev "$STA_IF" 2>/dev/null
                            iw dev "$STA_IF" disconnect 2>/dev/null; }
  # Put power save back the way we found it: a root script should not
  # permanently change the state of an adapter the operator uses for other
  # things. Only on the FINAL exit - cleanup() also runs between cells.
  [ -n "${STA_IF:-}" ] && [ "${PS_RESTORE:-}" = on ] && [ "${FINAL:-}" = 1 ] &&
    iw dev "$STA_IF" set power_save on 2>/dev/null
  # The MAC beacons autonomously, so a cell that died before its teardown can
  # leave one airing into the next cell. What silences it:
  #
  # An `authorized` toggle is NOT a power cycle - VBUS never drops and chip
  # state survives, which is why this tree's CLAUDE.md warns against calling it
  # cold. But on this part it does end the beacon, measured rather than assumed:
  # armed -> SSID seen; host process killed -> SSID STILL seen (the beacon is
  # autonomous); toggle -> SSID gone. The re-enumeration is what stops the
  # timer. That is all this needs to guarantee between cells, and it is all it
  # claims.
  #
  # For a genuine cold cycle, set AP_VBUS=<hubloc>:<port> and it uses uhubctl
  # the way tests/regress.py's REGRESS_VBUS_MAP does. Per-port-switchable HUB
  # ports only - never an xhci root port, which has wedged a device here badly
  # enough to need the machine powered off.
  #
  # Either way, confirmed against the VID:PID first: this runs as root and
  # writes to a path the caller supplied, and a stale AP_SYSFS would otherwise
  # yank whatever else is plugged there.
  if [ "$(cat "/sys/bus/usb/devices/$AP_SYSFS/idVendor" 2>/dev/null)" = "0e8d" ] &&
     [ "$(cat "/sys/bus/usb/devices/$AP_SYSFS/idProduct" 2>/dev/null)" = "7612" ]; then
    if [ -n "${AP_VBUS:-}" ]; then
      uhubctl -l "${AP_VBUS%%:*}" -p "${AP_VBUS##*:}" -a off >/dev/null 2>&1
      sleep 4
      uhubctl -l "${AP_VBUS%%:*}" -p "${AP_VBUS##*:}" -a on  >/dev/null 2>&1
      sleep 5
    else
      echo 0 > "/sys/bus/usb/devices/$AP_SYSFS/authorized" 2>/dev/null
      sleep 2
      echo 1 > "/sys/bus/usb/devices/$AP_SYSFS/authorized" 2>/dev/null
      sleep 3
    fi
  fi
}
trap cleanup EXIT INT TERM

# --- the station -----------------------------------------------------------
STA_IF=$(ls "/sys/bus/usb/devices/$STA_SYSFS:1.0/net/" 2>/dev/null | head -1)
if [ -z "$STA_IF" ]; then
  echo "$STA_SYSFS:1.0" > /sys/bus/usb/drivers_probe 2>/dev/null
  sleep 3
  STA_IF=$(ls "/sys/bus/usb/devices/$STA_SYSFS:1.0/net/" 2>/dev/null | head -1)
fi
if [ -z "$STA_IF" ]; then
  # drivers_probe is not enough after devourer has held this adapter: the
  # libusb claim leaves the interface on usbfs, and a re-probe (or a write to
  # a driver's `bind`) reports success without producing a netdev. Only a
  # re-enumeration does. This costs ~13 s, so it is the fallback, not the path.
  # Confirm something WiFi-shaped is actually at that path first. cleanup()
  # already guards the AP's toggle this way and says why: this runs as root and
  # writes to a path the caller supplied, so a stale STA_SYSFS re-enumerates
  # whatever else is plugged there - a keyboard, a disk. The station is not a
  # fixed VID:PID like the AP, so the check is "it has a USB interface 0 that
  # is not the AP", plus a refusal to touch a hub or the root device.
  sta_vid=$(cat "/sys/bus/usb/devices/$STA_SYSFS/idVendor" 2>/dev/null)
  sta_cls=$(cat "/sys/bus/usb/devices/$STA_SYSFS/bDeviceClass" 2>/dev/null)
  if [ -z "$sta_vid" ] || [ "$STA_SYSFS" = "$AP_SYSFS" ] || [ "$sta_cls" = "09" ]; then
    echo "refusing to re-enumerate $STA_SYSFS (vid='$sta_vid' class='$sta_cls')"
    echo "- it is not a plausible station device. Check STA_SYSFS."
    exit 2
  fi
  say "no netdev at $STA_SYSFS - re-enumerating the station ($sta_vid)"
  echo 0 > "/sys/bus/usb/devices/$STA_SYSFS/authorized" 2>/dev/null
  sleep 3
  echo 1 > "/sys/bus/usb/devices/$STA_SYSFS/authorized" 2>/dev/null
  sleep 10
  STA_IF=$(ls "/sys/bus/usb/devices/$STA_SYSFS:1.0/net/" 2>/dev/null | head -1)
fi
[ -n "$STA_IF" ] || { echo "no station iface at $STA_SYSFS (is a kernel driver bound?)"; exit 2; }
# A previous desktop/network-manager action can leave the newly probed PHY
# soft-blocked. `ip link set up` then fails silently below and wpa_supplicant
# exits without creating its pid file. Make the station precondition explicit.
rfkill unblock wlan 2>/dev/null || true
ip link set "$STA_IF" up 2>/dev/null || {
  echo "station iface $STA_IF could not be brought up (check rfkill)"; exit 2; }

# POWER SAVE OFF - a precondition these AP harnesses impose and had never
# stated, because THE AP CANNOT SERVE A POWER-SAVING STATION AT ALL.
#
# This is a capability gap, not a harness convenience. Verified by reading the
# three beacon builders (ap_responder.cpp:275-285, ap_wpa2.cpp:482-490,
# mt7612u_beacon_stop_check.cpp:62-86): NONE of them appends a TIM element,
# although src/sta/Dot11.h defines kEidTim. A beacon without a TIM is not a
# conforming AP beacon (802.11-2016 9.4.2.6) and gives a dozing station no
# DTIM schedule to wake on; nothing is buffered either, so a reply enqueued in
# on_rx and sent from the main loop airs while the station is asleep.
#
# Measured, open network, 60 pings at 1/s, one run per arm:
#
#   power_save on   ->  0/60 received, link dropped mid-run, AP saw 2 of ~62
#   power_save off  ->  60/60 received, 0% loss, rtt 0.735/1.530/7.644 ms
#
# That is ONE ORDERED PAIR, not an A/B, and the arms were run back to back in
# that order. It is strong enough to act on and NOT strong enough to retire
# the other symptoms this bench has recorded: the 524 ms / 1729 ms RTT was
# measured on the WPA2 path and is reproduced by NEITHER arm here (the PS-on
# arm received nothing at all, so it has no RTT), and "roughly half the runs
# carry no packet" is a population claim that a 0/60-vs-60/60 pair cannot
# explain by itself. Related, probably; explained, not yet.
#
# Consequence for the gate, stated plainly because it is easy to miss: a
# 14/14 obtained with power save forced off does NOT certify that this AP can
# serve a default-configured Linux station, because Linux defaults to
# power_save on. See docs/ap-mode.md's scope section.
#
# Not a hard exit: the `stop` cell has no data plane and does not care, and a
# station driver that does not implement the setting should not make the whole
# harness unrunnable. The two data-plane cells refuse instead, by name.
PS_OK=no
ps_err=$(iw dev "$STA_IF" set power_save off 2>&1) && PS_OK=yes
if [ "$PS_OK" = yes ]; then
  # Succeeding is not proof it took: a driver may accept and ignore it.
  case "$(iw dev "$STA_IF" get power_save 2>/dev/null)" in
    *off*) : ;;
    *)     PS_OK=no; ps_err="accepted the request but still reports it on" ;;
  esac
fi
[ "$PS_OK" = yes ] || say "WARNING: power save is not off on $STA_IF ($ps_err)"

# Restore it on the way out - this is a root script changing host state on an
# adapter the operator uses for other things.
PS_RESTORE=$(iw dev "$STA_IF" get power_save 2>/dev/null | grep -oE 'on|off' | head -1)
say "AP $AP_SYSFS   station $STA_SYSFS ($STA_IF)   ch$CH ($FREQ MHz)"

# `flush` is not optional: without it the BSS cache reports a beacon that
# stopped up to ~30 s ago as still present, which is how a broken StopBeacon
# reads as working.
# Scan up to three times and take the HIGHEST count. A scan can come back empty
# for its own reasons - colliding with another scan, a busy card, a dwell that
# misses a 100 TU beacon - and one empty result is not evidence of absence.
# Taking the max is the conservative reading in BOTH directions: it cannot turn
# a live beacon into a pass for "gone", and it stops a missed scan reporting a
# live beacon as absent. Observed: a "beacon not scannable" FAIL in a run where
# the station then associated, pinged, and got an auth at retry=0.
# seen() answers "how many BSS entries match"; it also reports how many BSS
# entries the scan saw AT ALL, and that second number is not a nicety. Every
# `= 0` caller treats 0 as the PASS value, so a scan that never ran, errored,
# or failed to parse is indistinguishable from a beacon that really stopped -
# and "the beacon is gone" is the only air witness this suite has that
# StopBeacon works. A scan returning nothing because the interface was busy
# (common seconds after a disconnect or a supplicant kill, and silenced by the
# 2>/dev/null below) would have scored a still-airing beacon as silenced.
#
# The station's netdev can DISAPPEAR MID-RUN: the adapter stays on the bus and
# authorized, but its interface 0 ends up with no driver bound and no netdev.
# Every scan after that returns nothing - which, before the block count below
# existed, the "beacon is gone" checks read as silence and scored as a PASS.
#
# Observed once on this bench, and the cause there was ANOTHER PROCESS holding
# the adapter (a second session claimed it), not a driver fault. That is the
# likeliest cause of this shape in general - devourer's own libusb claim
# detaches the kernel driver and leaves the interface on usbfs - so no driver
# bug is claimed here. Whatever the cause, the recovery and the reporting are
# the same, and drivers_probe is enough to bring it back.
#
# Say it loudly: a station that vanished mid-cell is a property of the bench,
# not of the AP under test, and must not be reported as either an AP failure
# or an AP success.
sta_check() {
  [ -d "/sys/class/net/$STA_IF" ] && return 0
  say "        (station $STA_IF VANISHED - no netdev; re-probing $STA_SYSFS:1.0)"
  say "        (if another process is using this adapter, that is the cause)"
  echo "$STA_SYSFS:1.0" > /sys/bus/usb/drivers_probe 2>/dev/null
  sleep 3
  if [ -d "/sys/class/net/$STA_IF" ]; then
    ip link set "$STA_IF" up 2>/dev/null
    iw dev "$STA_IF" set power_save off 2>/dev/null
    sleep 1
    say "        (station recovered - treat this run's timings as disturbed)"
    return 0
  fi
  say "        (station did NOT come back)"
  return 1
}

# Prints "<matches> <total_bss_blocks>". Use the on_air/gone wrappers.
seen() {   # $1 = SSID, $2 = BSSID, $3 = frequency or "any"
  local i n best=0 blocks=0 out b
  sta_check || { printf '0 0'; return; }
  for i in 1 2 3; do
    # Count BSS *entries*, keyed on all three of BSSID, SSID and the frequency
    # we actually scanned. Every part of that is load-bearing:
    #
    #  - BSSID and SSID together, because a neighbour running "devourerAP"
    #    would otherwise pass an arm check and fail a stop check;
    #  - one hit per BSS BLOCK, not per matching line: `iw` prints a separate
    #    "Information elements from Probe Response frame" section, so a BSS
    #    heard as both a beacon and a probe response prints `SSID:` twice;
    #  - the frequency, because the cache demonstrably holds MORE THAN ONE
    #    entry for a single BSSID. (A keying rule over BSSID/SSID/channel is
    #    the obvious explanation; it is not verified here, so it is not
    #    asserted.) Measured:
    #    a scan of 5180 alone reports our AP twice - once on 5180 with the
    #    real IEs, and once as a ghost carrying `freq: 2412`, `signal: 0.00`
    #    and an SSID-only IE set. Counting SSID lines across the whole scan
    #    therefore returned 2, and the caller's `= 1` read a beacon that was
    #    airing at -24 dBm as "not scannable".
    #
    #    That was ONE of the 5 GHz open cell's defects, not the whole story:
    #    the cell still failed at `iw connect` after this was fixed, and
    #    needed the pinned frequency+BSSID below as well. An earlier revision
    #    of this comment called it "the entire 5 GHz open-cell failure", which
    #    the existence of that second fix contradicts.
    #
    # The callers compare `-gt 0` / `= 0`, never `= 1`: how many cache entries
    # cfg80211 chooses to keep is not a property of the AP under test.
    out=$(iw dev "$STA_IF" scan flush freq "$FREQ" 2>/dev/null |
        awk -v b="$2" -v ss="$1" -v want="${3:-$FREQ}" '
          /^BSS / { k++; bssid[k] = tolower($2); sub(/\(.*/, "", bssid[k]); next }
          k > 0 && $1 == "freq:" { f[k] = $2 + 0; next }
          k > 0 { line = $0; sub(/^[ \t]+/, "", line)
                  sub(/[ \t\r]+$/, "", line)
                  if (line == "SSID: " ss) sname[k] = 1 }
          END { for (j = 1; j <= k; j++)
                  if (bssid[j] == tolower(b) && sname[j] &&
                      (want == "any" || f[j] == want + 0)) c++
                print (c + 0) " " (k + 0) }')
    n=${out%% *}; n=${n:-0}
    b=${out##* }; b=${b:-0}
    [ "$b" -gt "$blocks" ] && blocks=$b
    [ "$n" -gt "$best" ] && best=$n
    [ "$best" -gt 0 ] && break
    sleep 2
  done
  printf '%s %s' "$best" "$blocks"
}

# "The beacon is on the air, on the channel we told the AP to use." Pinned to
# $FREQ, which also fails an AP that beacons on the wrong channel.
on_air() {  # $1 = SSID, $2 = BSSID
  local r; r=$(seen "$1" "$2" "$FREQ")
  [ "${r%% *}" -gt 0 ]
}

# "Nothing with that identity is airing ANYWHERE." Deliberately NOT pinned to
# $FREQ. This bench has observed an entry for our own BSSID filed under a
# frequency we were not airing on, so a frequency filter could step straight
# over a beacon that is still transmitting and report it as stopped. And the
# scan must have seen at least one BSS, so a scan that did not run cannot
# masquerade as silence.
gone() {    # $1 = SSID, $2 = BSSID
  local r matches blocks
  r=$(seen "$1" "$2" any); matches=${r%% *}; blocks=${r##* }
  if [ "$blocks" -eq 0 ]; then
    say "        (the scan returned no BSS at all - refusing to read that as 'gone')"
    return 1
  fi
  [ "$matches" -eq 0 ]
}

apenv() {
  set -- DEVOURER_VID=0x0e8d DEVOURER_PID=0x7612 DEVOURER_CHANNEL="$CH" \
         DEVOURER_USB_BUS="${AP_SYSFS%%-*}" DEVOURER_USB_PORT="${AP_SYSFS#*-}" \
         DEVOURER_BCN_TU="$BCN_TU" DEVOURER_TX_WITH_RX=thread "$@"
  [ -n "$FW_DIR" ] && set -- DEVOURER_MT7612U_FW_DIR="$FW_DIR" "$@"
  printf '%s\n' "$@"
}

build() { # $1 = source stem, $2 = output name, $3.. = extra libs
  local src="$1" out="$2"; shift 2
  g++ -std=c++20 -O2 -I"$ROOT/src" -I"$ROOT/examples/common" \
      "$ROOT/tests/$src.cpp" "$ROOT/examples/common/env_config.cpp" \
      "$ROOT/examples/common/usb_select.cpp" \
      "$BUILD/libdevourer.a" $(pkg-config --cflags --libs libusb-1.0) \
      "$@" -lpthread -o "/tmp/$out" || return 1
}

# A cell must prove the AP CAME UP before it may believe any absence. A failed
# bring-up beacons nothing, which otherwise reads as a pass.
came_up() { grep -q "beaconing every" "$1"; }

# --- cell: open network ----------------------------------------------------
cell_open() {
  say "== open network (ap_responder) =="
  [ "${PS_OK:-no}" = yes ] || {
    bad "open: station power save is not off - this AP has no TIM/PS buffering, so the result would measure the station's sleep schedule, not the AP"
    return; }
  build ap_responder apr_onair || { bad "open: build"; return; }
  env $(apenv) timeout $((SECS + 20)) /tmp/apr_onair "$SECS" \
      >"$OUT/open.jsonl" 2>"$OUT/open.log" &
  local ap=$!; KIDS="$KIDS $ap"
  sleep 12
  came_up "$OUT/open.log" || { bad "open: AP did not come up (see $OUT/open.log)"; kill $ap 2>/dev/null; return; }
  ok "open: beacon armed"

  on_air "$AP_SSID" "$AP_BSSID" && ok "open: beacon on air" || bad "open: beacon not scannable"

  ip addr flush dev "$STA_IF" 2>/dev/null
  # Connect by SSID *and* frequency *and* BSSID. Connecting by SSID alone lets
  # the station pick any cache entry with that name, and there is reliably more
  # than one: while the AP is up, this station's cfg80211 cache carries a
  # second entry for our own BSSID showing `freq: 2412`, `signal: 0.00` and an
  # SSID-only IE set - no Supported Rates. Measured: 4 association attempts by
  # SSID alone, one failed, and on that one the AP logged NO auth and NO assoc
  # request at all while the station's driver said "No legacy rates in
  # association response".
  #
  # That is CONSISTENT with the station having targeted a cache entry which is
  # not this AP. It is not proof of it: an equally live reading is that the
  # station's AUTH simply never reached the AP's receiver, which this bench
  # sees often enough. Neither that entry's origin nor this failure's
  # mechanism is established here, and neither is claimed.
  #
  # Naming the frequency and the BSSID removes the ambiguity instead of racing
  # it, and is the more precise test anyway - it also fails an AP that beacons
  # on the wrong channel. But be clear about what it does NOT do: it changes
  # which BSS the STATION targets and changes nothing about what the AP
  # transmits, so if that second entry is produced by something this AP airs,
  # this makes the cell pass without explaining it.
  if timeout 30 iw dev "$STA_IF" connect -w "$AP_SSID" "$FREQ" "$AP_BSSID" >/dev/null 2>&1; then
    ok "open: station associated"
  else
    bad "open: station did not associate"; kill $ap 2>/dev/null; return
  fi

  ip addr add "$STAIP/24" dev "$STA_IF" 2>/dev/null
  ping -c 1 -W 2 -I "$STA_IF" "$APIP" >/dev/null 2>&1   # warm ARP
  if ping -c 6 -W 1 -I "$STA_IF" "$APIP" 2>&1 | tee "$OUT/open.ping" | grep -q " 0% packet loss"; then
    ok "open: data plane ($(grep -oE 'rtt [^ ]+ = [0-9./]+' "$OUT/open.ping" | head -1))"
  else
    bad "open: ping lost packets ($(grep -oE '[0-9]+% packet loss' "$OUT/open.ping" | head -1))"
  fi
  # THE AUTO-ACK WITNESS. This used to grep the AP's log for a management
  # frame logged at retry=0 and call that proof the MAC ACKed. It is not, and
  # the check could not fail:
  #
  #   ap_responder logs EVERY received copy of a frame, and reception does not
  #   depend on ACKing. Disarm the ACK responder entirely and the station's
  #   first AUTH still arrives with Retry clear, still gets logged at retry=0,
  #   and the grep still matches. The station then retransmits forever, but
  #   nothing looks at that. The predicate that discriminates is the ABSENCE of
  #   a retried copy - which is what docs/mt7612u-ap-mode.md's bring-up row
  #   ("3 auth frames, 0 retried") actually measured and this script never did.
  #
  # Worse, the previous revision widened the accepted set from AUTH-only to
  # AUTH-or-ASSOC after observing AUTH at retry=1 in four runs of five and
  # ASSOC at retry=0 in five of five. Under the check's own theory that 4/5 is
  # a finding, not noise; widening the predicate until the evidence stops
  # disagreeing is not the same bar, it is a weaker one. That reasoning is
  # withdrawn.
  #
  # What replaces it is measured at the station, which is the only side that
  # knows whether its frames were acknowledged: mac80211's per-peer tx_retries
  # and tx_failed. An AP that never ACKs forces the station's MAC to run the
  # full retry ladder on every frame and then give up, so both counters climb
  # hard. Measured against this AP over 48 post-association frames: retries 0,
  # failed 0.
  #
  # Two honest limits. These counters are created with the peer entry at
  # association, so they say nothing about the AUTH exchange that precedes it -
  # the AUTH-at-retry=1 asymmetry remains unexplained and is NOT closed by
  # this. And a station driver that does not export them cannot satisfy this
  # check; that is reported as a failure rather than waved through, because a
  # gate must not pass on absent evidence.
  sta_tx=$(iw dev "$STA_IF" station dump 2>/dev/null |
           awk '/tx packets:/ {p=$3} /tx retries:/ {r=$3} /tx failed:/ {f=$3}
                END { print (p=="" ? "-" : p), (r=="" ? "-" : r), (f=="" ? "-" : f) }')
  set -- $sta_tx
  tx_pkts="${1:--}"; tx_retries="${2:--}"; tx_failed="${3:--}"
  if [ "$tx_pkts" = "-" ] || [ "$tx_retries" = "-" ] || [ "$tx_failed" = "-" ]; then
    bad "open: station exports no tx_retries/tx_failed - no auto-ACK witness available"
  elif [ "$tx_pkts" -le 0 ]; then
    bad "open: station reported 0 transmitted frames - nothing to judge"
  elif [ "$tx_failed" -gt 0 ]; then
    bad "open: station gave up on $tx_failed frame(s) - the AP is not ACKing"
  elif [ "$tx_retries" -gt "$tx_pkts" ]; then
    bad "open: $tx_retries retries for $tx_pkts frames - the AP is barely ACKing"
  else
    ok "open: hardware auto-ACK (station: $tx_pkts frames, $tx_retries retries, $tx_failed failed)"
  fi

  # Only now tear the link down - the counters above are read from the live
  # peer entry and vanish with it.
  iw dev "$STA_IF" disconnect 2>/dev/null; ip addr flush dev "$STA_IF" 2>/dev/null
  wait $ap 2>/dev/null
  sleep 3
  gone "$AP_SSID" "$AP_BSSID" \
    && ok "open: nothing left airing after exit" \
    || bad "open: beacon STILL AIRING after exit"
}

# --- cell: WPA2-PSK --------------------------------------------------------
cell_wpa2() {
  say "== WPA2-PSK (ap_wpa2) =="
  [ "${PS_OK:-no}" = yes ] || {
    bad "wpa2: station power save is not off - see the open cell"
    return; }
  build ap_wpa2 apw_onair -lcrypto || { bad "wpa2: build"; return; }
  env $(apenv) DEVOURER_WPA2_PSK="$PSK" timeout $((SECS + 20)) /tmp/apw_onair "$SECS" \
      >"$OUT/wpa2.jsonl" 2>"$OUT/wpa2.log" &
  local ap=$!; KIDS="$KIDS $ap"
  sleep 12
  came_up "$OUT/wpa2.log" || { bad "wpa2: AP did not come up (see $OUT/wpa2.log)"; kill $ap 2>/dev/null; return; }
  ok "wpa2: beacon armed"

  local wpa="$OUT/wpa.conf"
  printf 'network={\n\tssid="devourerAP"\n\tpsk="%s"\n\tkey_mgmt=WPA-PSK\n\tproto=RSN\n\tpairwise=CCMP\n\tgroup=CCMP\n\tscan_ssid=1\n}\n' "$PSK" > "$wpa"
  # Address the interface BEFORE the supplicant starts. Some stations drop the
  # link on inactivity within seconds of a completed 4-way, and every step
  # between "handshake complete" and "first packet" is a chance to straddle
  # that timer. Configuring the address up front is free and removes one.
  ip addr flush dev "$STA_IF" 2>/dev/null
  ip addr add "$STAIP/24" dev "$STA_IF" 2>/dev/null
  # $OUT persists between runs, so a stale pid file would have this script kill
  # an unrelated, recycled PID as root. Remove it and require a fresh one.
  rm -f "$OUT/wpa.pid"
  if ! wpa_supplicant -i "$STA_IF" -c "$wpa" -P "$OUT/wpa.pid" \
         -f "$OUT/wpa-supplicant.log" -B >/dev/null 2>&1; then
    bad "wpa2: wpa_supplicant did not start (see $OUT/wpa-supplicant.log)"
    kill $ap 2>/dev/null; return
  fi
  KIDS="$KIDS $(cat "$OUT/wpa.pid" 2>/dev/null)"
  local i
  for i in $(seq 1 20); do
    grep -q "4-WAY HANDSHAKE COMPLETE" "$OUT/wpa2.log" && break
    sleep 1
  done
  if grep -q "4-WAY HANDSHAKE COMPLETE" "$OUT/wpa2.log"; then
    ok "wpa2: 4-way complete (MIC verified, station keyed)"
  else
    bad "wpa2: 4-way did not complete"
    kill "$(cat "$OUT/wpa.pid" 2>/dev/null)" 2>/dev/null
    kill $ap 2>/dev/null; return
  fi

  ping -c 1 -W 2 -I "$STA_IF" "$APIP" >/dev/null 2>&1
  if ping -c 6 -W 1 -I "$STA_IF" "$APIP" 2>&1 | tee "$OUT/wpa2.ping" | grep -q " 0% packet loss"; then
    ok "wpa2: encrypted data plane ($(grep -oE 'rtt [^ ]+ = [0-9./]+' "$OUT/wpa2.ping" | head -1))"
  else
    bad "wpa2: encrypted ping lost packets"
  fi

  kill "$(cat "$OUT/wpa.pid" 2>/dev/null)" 2>/dev/null
  ip addr flush dev "$STA_IF" 2>/dev/null
  wait $ap 2>/dev/null
  sleep 3
  gone "$AP_SSID" "$AP_BSSID" \
    && ok "wpa2: nothing left airing after exit" \
    || bad "wpa2: beacon STILL AIRING after exit"
}

# --- cell: software CCMP cost under real MediaTek RX/TX load ----------------
# This is intentionally not called a software/hardware A/B yet. An open AP is
# not a hardware-CCMP proxy; a genuine hardware arm must keep this traffic and
# measurement cell unchanged and swap only key/WCID + descriptor/RX handling.
cell_bench() {
  say "== software CCMP benchmark (${BENCH_PAYLOAD}B ping payload, ${BENCH_SECS}s) =="
  case "$BENCH_PAYLOAD" in
    ''|*[!0-9]*) bad "bench: BENCH_PAYLOAD must be an integer"; return ;;
  esac
  [ "$BENCH_PAYLOAD" -ge 0 ] && [ "$BENCH_PAYLOAD" -le 1400 ] || {
    bad "bench: BENCH_PAYLOAD must be 0..1400 (avoid IP fragmentation)"; return; }
  build ap_wpa2 apw_bench -lcrypto || { bad "bench: build"; return; }
  local run_secs=$((BENCH_SECS * 2 + 45))
  env $(apenv) DEVOURER_WPA2_PSK="$PSK" DEVOURER_CCMP_PROFILE=1 \
      timeout $((run_secs + 20)) /tmp/apw_bench "$run_secs" \
      >"$OUT/bench.jsonl" 2>"$OUT/bench.log" &
  local ap=$!; KIDS="$KIDS $ap"
  sleep 12
  came_up "$OUT/bench.log" || { bad "bench: AP did not come up"; kill $ap 2>/dev/null; return; }
  # `$ap` is coreutils timeout; CPU belongs to its direct apw_bench child.
  local ap_cpu_pid
  ap_cpu_pid=$(pgrep -P "$ap" -x apw_bench | head -1)
  [ -n "$ap_cpu_pid" ] || {
    bad "bench: cannot find apw_bench child of timeout"; kill $ap 2>/dev/null; return; }

  local wpa="$OUT/bench-wpa.conf"
  printf 'network={\n\tssid="devourerAP"\n\tpsk="%s"\n\tkey_mgmt=WPA-PSK\n\tproto=RSN\n\tpairwise=CCMP\n\tgroup=CCMP\n\tscan_ssid=1\n}\n' "$PSK" > "$wpa"
  ip addr flush dev "$STA_IF" 2>/dev/null
  wpa_supplicant -i "$STA_IF" -c "$wpa" -P "$OUT/bench-wpa.pid" \
      -f "$OUT/bench-wpa.log" -B >/dev/null 2>&1 || {
    bad "bench: wpa_supplicant did not start (see $OUT/bench-wpa.log)";
    kill $ap 2>/dev/null; return; }
  local wp; wp=$(cat "$OUT/bench-wpa.pid" 2>/dev/null); KIDS="$KIDS $wp"
  local i
  for i in $(seq 1 25); do
    grep -q "4-WAY HANDSHAKE COMPLETE" "$OUT/bench.log" && break
    sleep 1
  done
  grep -q "4-WAY HANDSHAKE COMPLETE" "$OUT/bench.log" || {
    bad "bench: 4-way did not complete"; kill "$wp" $ap 2>/dev/null; return; }
  ip addr add "$STAIP/24" dev "$STA_IF" 2>/dev/null
  ping -c 1 -W 2 -I "$STA_IF" "$APIP" >/dev/null 2>&1

  # Paired idle window removes beacon/RX-loop and host background CPU from the
  # system-wide number. /proc/<pid>/stat gives the AP itself; /proc/stat keeps
  # usbfs/xHCI softirq and worker work which process CPU would hide.
  proc_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null; }
  sys_busy() { awk '/^cpu / { idle=$5+$6; for(i=2;i<=NF;i++) total+=$i; print total-idle }' /proc/stat; }
  local hz; hz=$(getconf CLK_TCK)
  local ib0 ib1 wb0 wb1 ip0 ip1 wp0 wp1
  ib0=$(sys_busy); ip0=$(proc_ticks "$ap_cpu_pid"); sleep "$BENCH_SECS"
  ib1=$(sys_busy); ip1=$(proc_ticks "$ap_cpu_pid")
  wb0=$(sys_busy); wp0=$(proc_ticks "$ap_cpu_pid")
  ping -f -q -s "$BENCH_PAYLOAD" -w "$BENCH_SECS" -I "$STA_IF" "$APIP" \
      >"$OUT/bench.ping" 2>&1 || true
  wb1=$(sys_busy); wp1=$(proc_ticks "$ap_cpu_pid")

  local tx rx loss ap_core sys_cores
  tx=$(sed -n 's/^\([0-9][0-9]*\) packets transmitted.*/\1/p' "$OUT/bench.ping" | tail -1)
  rx=$(sed -n 's/.* \([0-9][0-9]*\) received.*/\1/p' "$OUT/bench.ping" | tail -1)
  loss=$(sed -n 's/.* \([0-9.][0-9.]*%\) packet loss.*/\1/p' "$OUT/bench.ping" | tail -1)
  tx=${tx:-0}; rx=${rx:-0}; loss=${loss:-unknown}
  ap_core=$(awk -v w="$((wp1-wp0))" -v i="$((ip1-ip0))" -v h="$hz" -v s="$BENCH_SECS" \
      'BEGIN { v=(w-i)/h/s*100; if(v<0)v=0; printf "%.2f",v }')
  sys_cores=$(awk -v w="$((wb1-wb0))" -v i="$((ib1-ib0))" -v h="$hz" -v s="$BENCH_SECS" \
      'BEGIN { v=(w-i)/h/s; if(v<0)v=0; printf "%.3f",v }')
  # This used to be an unconditional ok(): the cell printed PASS for 0/0
  # replies and 100% loss. The CPU numbers are a measurement and are reported
  # either way, but a cell has to be able to fail.
  if [ "${rx:-0}" -gt 0 ] && [ "$rx" -ge $(( tx / 2 )) ]; then
    ok "bench: $rx/$tx replies, loss=$loss, AP=${ap_core}% core, system_increment=${sys_cores} cores"
  else
    bad "bench: only $rx/$tx replies (loss=$loss) - too few to time anything; AP=${ap_core}% core"
  fi

  kill "$wp" 2>/dev/null
  ip addr flush dev "$STA_IF" 2>/dev/null
  wait $ap 2>/dev/null
  local profile; profile=$(grep '"ev":"ccmp.profile"' "$OUT/bench.log" | tail -1)
  [ -n "$profile" ] || { bad "bench: missing ccmp.profile"; return; }
  printf '%s\n' "$profile"
  python3 - "$profile" "$BENCH_PAYLOAD" "$BENCH_SECS" "$tx" "$rx" "$ap_core" "$sys_cores" <<'PYEOF'
import json, sys
p = json.loads(sys.argv[1])
def ns_per(which):
    n = p[f"{which}_frames"]
    return p[f"{which}_ns"] / n if n else 0
row = {"ev":"mt7612u.ccmp_bench", "path":p["path"],
       "ping_payload":int(sys.argv[2]), "seconds":int(sys.argv[3]),
       "requests":int(sys.argv[4]), "replies":int(sys.argv[5]),
       "reply_pps":int(sys.argv[5])/int(sys.argv[3]),
       "ap_incremental_core_pct":float(sys.argv[6]),
       "system_incremental_cores":float(sys.argv[7]),
       "ccmp_tx_ns_per_frame":ns_per("tx"),
       "ccmp_rx_ns_per_frame":ns_per("rx")}
print(json.dumps(row, separators=(",",":")))
PYEOF
}

# --- cell: the beacon lifecycle -------------------------------------------
cell_stop() {
  say "== beacon lifecycle (StartBeacon / StopBeacon / re-arm) =="
  build mt7612u_beacon_stop_check bstop_onair || { bad "stop: build"; return; }
  local phase=24
  env $(apenv) timeout $((phase * 3 + 40)) /tmp/bstop_onair "$phase" \
      >"$OUT/stop.log" 2>&1 &
  local ap=$!; KIDS="$KIDS $ap"

  # Wait for the ARM ITSELF, not for the phase banner. The banner prints
  # before StartBeacon, and the arm is not instant - it copies a 1600-byte
  # page over EP0 and reads the identity back. Sleeping a guessed interval
  # after the banner is how phase 1 of this very cell reported "armed but not
  # scannable" while phase 3, which happened to sleep longer, passed.
  # grep -c PRINTS 0 and EXITS 1 when it matches nothing, so `|| echo 0`
  # appends a second line and every later [ -gt ] dies on "0\n0".
  armed() {
    local n
    n=$(grep -c "beaconing every" "$OUT/stop.log" 2>/dev/null)
    printf '%s' "${n:-0}"
  }
  wait_arm() { # $1 = the count to exceed, $2 = seconds to wait
    local i
    for i in $(seq 1 "$2"); do [ "$(armed)" -gt "$1" ] && return 0; sleep 1; done
    return 1
  }

  wait_arm 0 30 || { bad "stop: never armed"; kill $ap 2>/dev/null; return; }
  sleep 4
  on_air "$STOP_SSID" "$STOP_BSSID" && ok "stop: armed - beacon on air" || bad "stop: armed but not scannable"

  local n_arms; n_arms=$(armed)
  local i
  for i in $(seq 1 60); do grep -q "PHASE 2" "$OUT/stop.log" && break; sleep 1; done
  sleep 6
  gone "$STOP_SSID" "$STOP_BSSID" && ok "stop: stopped - beacon gone" || bad "stop: STILL AIRING after StopBeacon"

  # The re-arm is the same non-instant operation: wait for the second
  # "beaconing every", not for the banner that precedes it.
  wait_arm "$n_arms" 60 || { bad "stop: re-arm never reported"; kill $ap 2>/dev/null; return; }
  sleep 4
  on_air "$STOP_SSID" "$STOP_BSSID" && ok "stop: re-armed - beacon back" || bad "stop: re-arm did not air"

  wait $ap 2>/dev/null
  grep -q "0 failure(s)" "$OUT/stop.log" \
    && ok "stop: local contract checks (2nd stop false, update-with-no-beacon false, update accepts an unchanged payload and refuses a changed addr3 - and still refuses a changed addr2)" \
    || bad "stop: local contract checks failed"
}

# Expected check counts, so the advertised score is machine-enforced instead
# of counted by eye. A future edit that dropped an ok()/bad() pair would
# otherwise run 13 checks, exit 0, and still be reported as "14/14".
case "$CELLS" in
  open)  cell_open;  want=6 ;;
  wpa2)  cell_wpa2;  want=4 ;;
  stop)  cell_stop;  want=4 ;;
  bench) cell_bench; want=1 ;;
  all)   cell_open; cleanup; cell_wpa2; cleanup; cell_stop; want=14 ;;
  *)     echo "usage: $0 [open|wpa2|stop|bench|all]"; exit 2 ;;
esac
FINAL=1

say ""
say "=== $pass passed, $fail failed   (logs: $OUT) ==="
say "=== station $STA_IF on ch$CH/$FREQ, BCN_TU=$BCN_TU, power_save=$([ "${PS_OK:-no}" = yes ] && echo off || echo UNKNOWN) ==="
if [ $(( pass + fail )) -ne "$want" ]; then
  say "=== HARNESS ERROR: ran $(( pass + fail )) checks, expected $want ==="
  exit 2
fi
exit $(( fail > 0 ))
