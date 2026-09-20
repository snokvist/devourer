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
# BCN_TU is the beacon interval and it is a STATION property, not an AP one.
# The default 100 is what the MediaTek station tolerated. A supplicant whose
# scan hops channels fast enough can miss a 100 TU beacon entirely - the
# RTL8812AU here does - and the run then reads as "AP never came up" when the
# AP was airing the whole time. Lower it rather than believe that.
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
FREQ=$(( CH < 15 ? 2407 + CH * 5 : 5000 + CH * 5 ))
PSK="${PSK:-devourer123}"
BCN_TU="${BCN_TU:-100}"
SECS="${SECS:-40}"
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
  say "no netdev at $STA_SYSFS - re-enumerating the station"
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

# POWER SAVE OFF, and this is not a convenience - it is a precondition these
# AP harnesses impose and had never stated.
#
# Neither ap_responder nor ap_wpa2 implements 802.11 power save: the beacon
# carries no TIM bitmap, nothing is buffered for a dozing station, and every
# reply is injected the instant the request is parsed. A station in PS is
# asleep when that reply airs and never hears it. Measured on this bench, open
# network, 60 pings at 1/s, changing NOTHING but this line:
#
#   power_save on   ->  0/60 received, link dropped mid-run, AP saw 2 of ~62
#                       data frames; earlier runs 50% loss with a 942 ms outlier
#   power_save off  ->  60/60 received, 0% loss, rtt 0.735/1.530/7.644 ms
#
# That one setting is what produced every symptom this bench had been carrying
# as unexplained: runs that complete a 4-way and then never pass a packet,
# "the station drops on inactivity", and an RTT of 524 ms mean / 1729 ms max.
# None of it was the radio, the link budget, or the crypto.
#
# Left as a hard requirement rather than a fallback: a harness that silently
# tolerated PS would be measuring the station's sleep schedule, not the AP.
if ! iw dev "$STA_IF" set power_save off 2>/dev/null; then
  echo "could not disable power save on $STA_IF - these AP harnesses have no"
  echo "TIM/PS buffering, so a dozing station will read as a data-plane failure"
  exit 2
fi
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
seen() {   # $1 = SSID, $2 = BSSID  -> number of matching BSS entries
  local i n best=0
  for i in 1 2 3; do
    # Count BSS *entries*, keyed on all three of BSSID, SSID and the frequency
    # we actually scanned. Every part of that is load-bearing:
    #
    #  - BSSID and SSID together, because a neighbour running "devourerAP"
    #    would otherwise pass an arm check and fail a stop check;
    #  - one hit per BSS BLOCK, not per matching line: `iw` prints a separate
    #    "Information elements from Probe Response frame" section, so a BSS
    #    heard as both a beacon and a probe response prints `SSID:` twice;
    #  - the frequency, because cfg80211 keys its cache by (BSSID, SSID,
    #    channel) and will hold SEVERAL entries for one BSSID. Measured here:
    #    a scan of 5180 alone reports our AP twice - once on 5180 with the
    #    real IEs, and once as a ghost carrying `freq: 2412`, `signal: 0.00`
    #    and an SSID-only IE set that survives `flush` and outlives the AP
    #    process. Counting SSID lines across the whole scan therefore returned
    #    2, and the caller's `= 1` read a beacon that was airing at -24 dBm as
    #    "not scannable". That was the entire 5 GHz open-cell failure.
    #
    # The callers compare `-gt 0` / `= 0`, never `= 1`: how many cache entries
    # cfg80211 chooses to keep is not a property of the AP under test.
    n=$(iw dev "$STA_IF" scan flush freq "$FREQ" 2>/dev/null |
        awk -v b="$2" -v ss="$1" -v want="$FREQ" '
          /^BSS / { k++; bssid[k] = tolower($2); sub(/\(.*/, "", bssid[k]); next }
          k > 0 && $1 == "freq:" { f[k] = $2 + 0; next }
          k > 0 { line = $0; sub(/^[ \t]+/, "", line)
                  if (line == "SSID: " ss) sname[k] = 1 }
          END { for (j = 1; j <= k; j++)
                  if (bssid[j] == tolower(b) && f[j] == want + 0 && sname[j]) c++
                print c + 0 }')
    n=${n:-0}
    [ "$n" -gt "$best" ] && best=$n
    [ "$best" -gt 0 ] && break
    sleep 2
  done
  printf '%s' "$best"
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
  build ap_responder apr_onair || { bad "open: build"; return; }
  env $(apenv) timeout $((SECS + 20)) /tmp/apr_onair "$SECS" \
      >"$OUT/open.jsonl" 2>"$OUT/open.log" &
  local ap=$!; KIDS="$KIDS $ap"
  sleep 12
  came_up "$OUT/open.log" || { bad "open: AP did not come up (see $OUT/open.log)"; kill $ap 2>/dev/null; return; }
  ok "open: beacon armed"

  [ "$(seen devourerAP 02:42:75:05:d6:00)" -gt 0 ] && ok "open: beacon on air" || bad "open: beacon not scannable"

  ip addr flush dev "$STA_IF" 2>/dev/null
  # Connect by SSID *and* frequency *and* BSSID. Connecting by SSID alone lets
  # the station pick any cache entry with that name, and there is reliably more
  # than one: while the AP is up, this station's cfg80211 cache carries a
  # second entry for our own BSSID showing `freq: 2412`, `signal: 0.00` and an
  # SSID-only IE set - no Supported Rates. Measured: 4 association attempts by
  # SSID alone, one failed, and on that one the AP logged NO auth and NO assoc
  # request at all while the station's driver said "No legacy rates in
  # association response". It had associated with something that was not this
  # AP. Where that entry comes from is NOT established here and is not claimed.
  #
  # Naming the frequency and the BSSID removes the ambiguity instead of racing
  # it, and is the more precise test anyway: the cell knows exactly which BSS
  # it put on the air.
  if timeout 30 iw dev "$STA_IF" connect -w devourerAP "$FREQ" 02:42:75:05:d6:00 >/dev/null 2>&1; then
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
  # retry=0 on a management frame IS the hardware ACK: a frame the AP did not
  # ACK is retransmitted by the station's MAC with FC Retry set. This is the
  # only evidence that the APC slot and port identity are both right.
  #
  # It must NOT be pinned to AUTH. WHICH frame arrives at retry=0 is a property
  # of the station and of chance, not of the AP. Measured on this bench's
  # RTL8812AU: five runs, AUTH arrived at retry=1 in four of them and retry=0
  # in the fifth, with ASSOC at retry=0 in all five. Power save on/off does not
  # predict it - one PS-off run with 60/60 pings and 0% loss still showed AUTH
  # at retry=1. mac80211 on that station logs "send auth (try 1/3)" then
  # "authenticated", so the retransmission happened in hardware inside a single
  # management attempt: the first copy was not ACKed, the second was.
  #
  # So the AUTH-specific form is a coin flip on this station, and it failed a
  # link that then passed every other check. Either frame proves the same thing
  # - both are unicast to the AP's MAC and both must be ACKed by it - so accept
  # either. This is not a weaker bar; it is the same bar without the accident
  # of which frame happened to survive its first transmission.
  if grep -qE "(AUTH|ASSOC) req from .* retry=0" "$OUT/open.log"; then
    ok "open: hardware auto-ACK ($(grep -oE '(AUTH|ASSOC) req from [^ ]* .*retry=0' "$OUT/open.log" | head -1 | grep -oE '^(AUTH|ASSOC)') at retry=0)"
  else
    bad "open: no management frame at retry=0 - the MAC did not ACK"
  fi

  iw dev "$STA_IF" disconnect 2>/dev/null; ip addr flush dev "$STA_IF" 2>/dev/null
  wait $ap 2>/dev/null
  sleep 3
  [ "$(seen devourerAP 02:42:75:05:d6:00)" = 0 ] \
    && ok "open: nothing left airing after exit" \
    || bad "open: beacon STILL AIRING after exit"
}

# --- cell: WPA2-PSK --------------------------------------------------------
cell_wpa2() {
  say "== WPA2-PSK (ap_wpa2) =="
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
  wpa_supplicant -i "$STA_IF" -c "$wpa" -P "$OUT/wpa.pid" -B >/dev/null 2>&1
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
  [ "$(seen devourerAP 02:42:75:05:d6:00)" = 0 ] \
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
  ok "bench: $rx/$tx replies, loss=$loss, AP=${ap_core}% core, system_increment=${sys_cores} cores"

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
  [ "$(seen mtStopCheck 02:4d:54:53:54:50)" -gt 0 ] && ok "stop: armed - beacon on air" || bad "stop: armed but not scannable"

  local n_arms; n_arms=$(armed)
  local i
  for i in $(seq 1 60); do grep -q "PHASE 2" "$OUT/stop.log" && break; sleep 1; done
  sleep 6
  [ "$(seen mtStopCheck 02:4d:54:53:54:50)" = 0 ] && ok "stop: stopped - beacon gone" || bad "stop: STILL AIRING after StopBeacon"

  # The re-arm is the same non-instant operation: wait for the second
  # "beaconing every", not for the banner that precedes it.
  wait_arm "$n_arms" 60 || { bad "stop: re-arm never reported"; kill $ap 2>/dev/null; return; }
  sleep 4
  [ "$(seen mtStopCheck 02:4d:54:53:54:50)" -gt 0 ] && ok "stop: re-armed - beacon back" || bad "stop: re-arm did not air"

  wait $ap 2>/dev/null
  grep -q "0 failure(s)" "$OUT/stop.log" \
    && ok "stop: local contract checks (2nd stop false, update-with-no-beacon false, update accepts an unchanged payload and refuses a changed addr3 - and still refuses a changed addr2)" \
    || bad "stop: local contract checks failed"
}

case "$CELLS" in
  open) cell_open ;;
  wpa2) cell_wpa2 ;;
  stop) cell_stop ;;
  bench) cell_bench ;;
  all)  cell_open; cleanup; cell_wpa2; cleanup; cell_stop ;;
  *)    echo "usage: $0 [open|wpa2|stop|bench|all]"; exit 2 ;;
esac

say ""
say "=== $pass passed, $fail failed   (logs: $OUT) ==="
exit $(( fail > 0 ))
