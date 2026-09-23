#!/usr/bin/env bash
# sta_d2d_onair.sh — devourer to devourer. No kernel 802.11 anywhere.
#
# tests/mt7612u_sta_onair.sh joins hostapd, on purpose: an independent
# implementation on independent silicon is the witness that found the group
# rekey defect. THIS harness is the other half of Phase 5 and the shape this
# project actually ships - an MT7612U running tests/sta_client.cpp joining an
# RTL8812CU running tests/ap_wpa2.cpp, with the whole 802.11 stack in
# userspace on both ends.
#
# WHAT IT BUYS THAT THE HOSTAPD HARNESS CANNOT:
#
#   - 5 GHz. The kernel AP harness is stuck on ch6 because the RTL8812AU's
#     5 GHz channels are all `no IR` in this regulatory domain, so hostapd
#     refuses to serve them. devourer programs its own synthesizer and asks
#     no one, so both ends simply tune. THE OPERATOR OWNS COMPLIANCE - that
#     is this project's standing position (see CLAUDE.md), and it is the only
#     reason this cell exists.
#   - The AP's DOWNLINK through its TAP. Phase 2b.8 closed on a narrated
#     bench run; nothing scripted has ever driven `DEVOURER_AP_TAP` until
#     now. The `AP -> station` ping is that path.
#   - Both ledgers at once. Two independent accounts of the same frames.
#
# THE AP RUNS IN A NETWORK NAMESPACE, for the same reason the other harness
# puts the kernel AP in one: both TAPs are on one host, and with both in the
# root namespace the kernel routes between them locally and the ping passes
# with the antennas unplugged. Here it is CHEAP and SAFE - devourer holds the
# radio over libusb, so there is no phy to move and none to destroy. The
# netns holds nothing but a TAP that dies with the process, and `ip netns
# del` cannot cost an adapter the way it can in mt7612u_sta_onair.sh.
#
# Cells:
#
#   wpa2      the 2.4 GHz link  (CH,  default 6)
#   fiveghz   the same at 5 GHz (CH5, default 36) - unreachable with a kernel AP
#   airgap    the two radios on DIFFERENT channels: the link MUST NOT form,
#             and the ping MUST be lost. The falsifier for every cell above.
#   bench     software CCMP cost at BOTH ends, under a load the link sustains
#   flood     the ceiling, and whether both ledgers account for what exceeds it
#
#   sudo tests/sta_d2d_onair.sh                # wpa2 + fiveghz + airgap
#   sudo CH=6 tests/sta_d2d_onair.sh wpa2
#   sudo CH5=149 tests/sta_d2d_onair.sh fiveghz
#   sudo tests/sta_d2d_onair.sh bench
#   sudo tests/sta_d2d_onair.sh flood
#
# Env: STA_SYSFS (the MT7612U sta_client claims), AP_SYSFS (the adapter
# ap_wpa2 claims), AP_VID/AP_PID, CH, CH5, PSK, FW_DIR, SECS, NS, APTAP,
# STATAP, AIRGAP_SECS, BENCH_SECS, BENCH_PAYLOAD, BENCH_PPS.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CELLS="${1:-all}"

STA_SYSFS="${STA_SYSFS:-1-1}"
AP_SYSFS="${AP_SYSFS:-5-1}"
AP_VID="${AP_VID:-0x0bda}"
AP_PID="${AP_PID:-0xc812}"
CH="${CH:-6}"
CH5="${CH5:-36}"
PSK="${PSK:-devourer123}"
# NOT CONFIGURABLE, and not ours to choose: tests/ap_wpa2.cpp hardcodes both,
# and the PMK is PBKDF2 over the SSID, so a mismatch is a four-way that fails
# for a reason no counter names. Checked against the AP below rather than
# trusted - see the identity check in the cell.
SSID=devourerAP
BSSID=02:42:75:05:d6:00
FW_DIR="${FW_DIR:-/lib/firmware/mediatek}"
# Two userspace bring-ups, in series: ~12 s for the Jaguar3 AP (tables, IQK,
# DACK) and ~15 s for the MT7612U station (firmware load, IQK). The budget is
# measured from process start, so it covers both plus the waits in front of
# the measurement - the same trap mt7612u_sta_onair.sh documents at SECS.
SECS="${SECS:-75}"
NS="${NS:-d2dap}"
APTAP="${APTAP:-dvap0}"
STATAP="${STATAP:-dvsta0}"
# The falsifier needs no association, so it needs no budget for one - only
# the two bring-ups, the 25 s it waits to be sure nothing joined, and the
# ping that has to be lost.
AIRGAP_SECS="${AIRGAP_SECS:-60}"
BENCH_SECS="${BENCH_SECS:-15}"
# The bench PACES its traffic, and that is a finding rather than a
# convenience: this link's measured ceiling is about twenty round trips a
# second (see the flood cell), so timing a cipher under a ~100 pps flood
# times a link that is collapsing.
BENCH_PPS="${BENCH_PPS:-10}"
BENCH_PAYLOAD="${BENCH_PAYLOAD:-1400}"
# A subnet of its own, so a stale address left by mt7612u_sta_onair.sh
# (192.168.98.0/24) or by the AP demos (192.168.99.0/24) cannot supply a
# second route to the address under test.
APIP=192.168.97.1
STAIP=192.168.97.2
OUT="${OUT:-/tmp/devourer-sta-d2d}"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
mkdir -p "$OUT"
pass=0; fail=0
say() { printf '%s\n' "$*"; }
ok()  { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

chan_freq() {   # $1 = channel -> MHz on stdout, empty if unmappable
  case "$1" in
    ''|*[!0-9]*) return 1 ;;
  esac
  if   [ "$1" -ge 1 ]  && [ "$1" -le 13 ];  then echo $(( 2407 + $1 * 5 ))
  elif [ "$1" = 14 ];                       then echo 2484
  elif [ "$1" -ge 34 ] && [ "$1" -le 196 ]; then echo $(( 5000 + $1 * 5 ))
  else return 1
  fi
}

AP_PID_RUN=""; STA_PID_RUN=""
NM_TAP=""

cleanup() {
  # The AP first and with SIGTERM, not SIGKILL. ap_wpa2 beacons AUTONOMOUSLY -
  # the chip does it, not the process - so only its own exit path calls
  # StopBeacon. A killed AP leaves a beacon on the air until the adapter is
  # re-enumerated, and leaves no ledger for the cell to grade.
  [ -n "$STA_PID_RUN" ] && kill "$STA_PID_RUN" 2>/dev/null
  [ -n "$AP_PID_RUN" ]  && kill "$AP_PID_RUN"  2>/dev/null
  wait 2>/dev/null
  ip addr flush dev "$STATAP" 2>/dev/null
  ip link del "$STATAP" 2>/dev/null
  # Safe here in a way it is NOT in mt7612u_sta_onair.sh: nothing but a TAP
  # ever enters this namespace, because devourer holds both radios over
  # libusb and neither has a phy to move.
  ip netns del "$NS" 2>/dev/null
}
trap cleanup EXIT INT TERM

# --- preflight -------------------------------------------------------------

[ "$AP_SYSFS" != "$STA_SYSFS" ] || {
  echo "AP_SYSFS and STA_SYSFS are both $AP_SYSFS - this harness needs two adapters"
  exit 2; }

# Checked by VID:PID, because this runs as root and drives whatever is at the
# path it is handed. Both binaries select their adapter by USB topology
# (DEVOURER_USB_BUS/PORT), so a wrong path here is not a failed open - it is
# the WRONG RADIO, opened successfully.
sta_vid=$(cat "/sys/bus/usb/devices/$STA_SYSFS/idVendor" 2>/dev/null)
sta_pid=$(cat "/sys/bus/usb/devices/$STA_SYSFS/idProduct" 2>/dev/null)
[ "$sta_vid" = "0e8d" ] && [ "$sta_pid" = "7612" ] || {
  echo "STA_SYSFS=$STA_SYSFS is not an MT7612U (found '$sta_vid:$sta_pid')"
  exit 2; }
ap_vid=$(cat "/sys/bus/usb/devices/$AP_SYSFS/idVendor" 2>/dev/null)
ap_pid=$(cat "/sys/bus/usb/devices/$AP_SYSFS/idProduct" 2>/dev/null)
want_vid=$(printf '%04x' "$AP_VID"); want_pid=$(printf '%04x' "$AP_PID")
[ "$ap_vid" = "$want_vid" ] && [ "$ap_pid" = "$want_pid" ] || {
  echo "AP_SYSFS=$AP_SYSFS is $ap_vid:$ap_pid, not the $want_vid:$want_pid this run expects"
  echo "(tests/ap_wpa2.cpp's AP-mode path is validated on Jaguar3 - 0bda:c812)"
  exit 2; }

rfkill unblock wlan 2>/dev/null || true

say "AP  $AP_SYSFS ($ap_vid:$ap_pid, devourer/ap_wpa2, in netns '$NS')"
say "STA $STA_SYSFS ($sta_vid:$sta_pid, devourer/sta_client, root namespace)"
say "ssid '$SSID' bssid $BSSID  psk '$PSK'  taps '$APTAP'/'$STATAP'"

# --- build -----------------------------------------------------------------

build_both() {
  [ -f "$BUILD/libdevourer.a" ] || {
    say "no $BUILD/libdevourer.a - build the library first"; return 1; }
  local cf; cf=$(pkg-config --cflags --libs libusb-1.0) || return 1
  local f
  for f in ap_wpa2 sta_client; do
    g++ -std=c++20 -O2 -I"$ROOT/src" -I"$ROOT/tests" -I"$ROOT/examples/common" \
        "$ROOT/tests/$f.cpp" "$ROOT/examples/common/env_config.cpp" \
        "$ROOT/examples/common/usb_select.cpp" "$BUILD/libdevourer.a" \
        $cf -lcrypto -lpthread -o "$OUT/$f" || return 1
  done
}

# --- the AP ----------------------------------------------------------------

ns_up() {
  ip netns list 2>/dev/null | grep -q "^$NS" && return 0
  ip netns add "$NS"
}

ap_up() {   # $1 = channel, $2 = seconds, $3.. = extra env
  local chan="$1" secs="$2"; shift 2
  rm -f "$OUT/ap.log"
  ip netns exec "$NS" env \
      DEVOURER_VID="$AP_VID" DEVOURER_PID="$AP_PID" \
      DEVOURER_USB_BUS="${AP_SYSFS%%-*}" DEVOURER_USB_PORT="${AP_SYSFS#*-}" \
      DEVOURER_CHANNEL="$chan" DEVOURER_WPA2_PSK="$PSK" DEVOURER_BCN_TU=25 \
      DEVOURER_TX_WITH_RX=thread DEVOURER_AP_TAP="$APTAP" "$@" \
      timeout $((secs + 25)) "$OUT/ap_wpa2" "$secs" >"$OUT/ap.log" 2>&1 &
  AP_PID_RUN=$!
  local i
  for i in $(seq 1 25); do
    ip netns exec "$NS" ip link show "$APTAP" >/dev/null 2>&1 && break
    kill -0 "$AP_PID_RUN" 2>/dev/null || return 1
    sleep 1
  done
  ip netns exec "$NS" ip link show "$APTAP" >/dev/null 2>&1 || return 1
  # THE AP'S TAP MUST CARRY THE BSSID. The AP classifies a received frame by
  # its DA: the BSSID's own address is "for this AP" and goes up to the host,
  # anything else is off-BSS. A TAP with the kernel's random MAC still works -
  # off-BSS is forwarded too - but it is working by the fallback path, and the
  # cell would be grading the wrong branch.
  ip netns exec "$NS" ip link set "$APTAP" address "$BSSID" || return 1
  ip netns exec "$NS" ip link set "$APTAP" up || return 1
  ip netns exec "$NS" ip addr add "$APIP/24" dev "$APTAP" 2>/dev/null
  for i in $(seq 1 40); do
    grep -q "ap_wpa2 up:" "$OUT/ap.log" 2>/dev/null && break
    kill -0 "$AP_PID_RUN" 2>/dev/null || return 1
    sleep 1
  done
  grep -q "beacon=OK" "$OUT/ap.log" 2>/dev/null
}

# --- the station -----------------------------------------------------------

sta_up() {   # $1 = channel, $2 = seconds, $3.. = extra env
  local chan="$1" secs="$2"; shift 2
  rm -f "$OUT/sta.log"
  env DEVOURER_VID=0x0e8d DEVOURER_PID=0x7612 \
      DEVOURER_USB_BUS="${STA_SYSFS%%-*}" DEVOURER_USB_PORT="${STA_SYSFS#*-}" \
      DEVOURER_CHANNEL="$chan" DEVOURER_TX_WITH_RX=thread \
      DEVOURER_MT7612U_FW_DIR="$FW_DIR" \
      DEVOURER_STA_SSID="$SSID" DEVOURER_STA_PSK="$PSK" \
      DEVOURER_STA_TAP="$STATAP" "$@" \
      timeout $((secs + 20)) "$OUT/sta_client" "$secs" >"$OUT/sta.log" 2>&1 &
  STA_PID_RUN=$!
  local i
  for i in $(seq 1 30); do
    grep -q "sta_client up:" "$OUT/sta.log" 2>/dev/null && return 0
    kill -0 "$STA_PID_RUN" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}

# Bring the station's TAP up and PROVE the route leaves through it.
#
# This is the assertion the whole harness rests on. Both radios are on one
# host; if 192.168.97.1 were reachable through anything else the ping would
# measure the kernel and pass with the antennas unplugged. The AP's own TAP
# is in $NS, which is what makes the root namespace's answer meaningful.
sta_tap_up() {
  local i
  for i in $(seq 1 20); do
    [ -d "/sys/class/net/$STATAP" ] && break
    sleep 1
  done
  [ -d "/sys/class/net/$STATAP" ] || { bad "the station created no TAP"; return 1; }
  command -v nmcli >/dev/null 2>&1 &&
    nmcli device set "$STATAP" managed no >/dev/null 2>&1 && NM_TAP=yes
  ip link set "$STATAP" up 2>/dev/null
  ip addr flush dev "$STATAP" 2>/dev/null
  ip addr add "$STAIP/24" dev "$STATAP" 2>/dev/null
  sleep 1
  local route; route=$(ip route get "$APIP" 2>/dev/null)
  case "$route" in
    *"dev $STATAP"*) return 0 ;;
    *) bad "the route to $APIP does not leave through $STATAP - a ping would not touch the air ($route)"
       return 1 ;;
  esac
}

wait_for() {   # $1 = pattern, $2 = file, $3 = seconds
  local i
  for i in $(seq 1 "$3"); do
    grep -q "$1" "$2" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

sta_alive() { kill -0 "$STA_PID_RUN" 2>/dev/null; }
ap_alive()  { kill -0 "$AP_PID_RUN" 2>/dev/null; }

# One field out of the station's exit ledger.
led()   { sed -n "s/.*$1=\\([0-9][0-9]*\\).*/\\1/p" "$OUT/sta.log" | tail -1; }
# ... and out of the AP's.
apled() { sed -n "s/.*$1=\\([0-9][0-9]*\\).*/\\1/p" "$OUT/ap.log" | tail -1; }

# A ping, with "did the thing that was supposed to be sending it still exist"
# built in. A cell that measures a data plane after one end has exited reports
# PACKET LOSS, which names the link - and the link was fine.
# $1 = cell, $2 = label, $3 = "up" | "down"
ping_cell() {
  local cell="$1" label="$2" dir="$3"
  if ! sta_alive || ! ap_alive; then
    bad "$cell: $label - an endpoint exited before the data plane could be measured (raise SECS, currently $SECS)"
    return 1
  fi
  if [ "$dir" = up ]; then
    ping -c 1 -W 3 -I "$STATAP" "$APIP" >/dev/null 2>&1
    ping -c 6 -W 2 -I "$STATAP" "$APIP" >"$OUT/$cell.$dir.ping" 2>&1
  else
    ip netns exec "$NS" ping -c 1 -W 3 -I "$APTAP" "$STAIP" >/dev/null 2>&1
    ip netns exec "$NS" ping -c 6 -W 2 -I "$APTAP" "$STAIP" >"$OUT/$cell.$dir.ping" 2>&1
  fi
  if ! sta_alive || ! ap_alive; then
    bad "$cell: $label - an endpoint exited DURING the measurement (raise SECS, currently $SECS)"
    return 1
  fi
  if grep -q " 0% packet loss" "$OUT/$cell.$dir.ping"; then
    ok "$cell: $label ($(grep -oE 'rtt [^ ]+ = [0-9./]+' "$OUT/$cell.$dir.ping" | head -1))"
    return 0
  fi
  bad "$cell: $label lost packets ($(grep -oE '[0-9.]+% packet loss' "$OUT/$cell.$dir.ping" | head -1))"
  return 1
}

# Stop both ends cleanly and wait for their ledgers. SIGTERM, never SIGKILL:
# both binaries answer it by leaving the loop, and it is that exit path that
# deauthenticates, prints the ledger and stops the beacon.
stop_both() {
  [ -n "$STA_PID_RUN" ] && kill "$STA_PID_RUN" 2>/dev/null
  wait "$STA_PID_RUN" 2>/dev/null
  STA_PID_RUN=""
  [ -n "$AP_PID_RUN" ] && kill "$AP_PID_RUN" 2>/dev/null
  wait "$AP_PID_RUN" 2>/dev/null
  AP_PID_RUN=""
}

# --- the link cell, run once per band --------------------------------------

cell_link() {   # $1 = cell name, $2 = channel
  local cell="$1" chan="$2" freq
  freq=$(chan_freq "$chan") || { bad "$cell: channel '$chan' is not one this harness can map"; return; }
  say "== $cell: devourer station <-> devourer AP on ch$chan ($freq MHz) =="

  build_both || { bad "$cell: build"; return; }
  ns_up      || { bad "$cell: could not create netns $NS"; return; }
  ap_up "$chan" $((SECS + 30)) || {
    bad "$cell: the devourer AP did not come up (see $OUT/ap.log)"; stop_both; return; }
  ok "$cell: the AP is beaconing on ch$chan"

  sta_up "$chan" "$SECS" || {
    bad "$cell: sta_client did not start (see $OUT/sta.log)"; stop_both; return; }
  sta_tap_up || { stop_both; return; }
  ok "$cell: the station's TAP is up and the route leaves through it"

  # THE TWO ENDS AGREE ABOUT WHO THE AP IS. sta_client prints the BSSID it
  # armed the MAC filter for, and this harness gave the AP's TAP that same
  # address by hand. If ap_wpa2's kBssid ever moves, this is what says so -
  # otherwise the symptom is a downlink that silently takes the off-BSS
  # branch, which still works, so nothing else here would notice.
  if wait_for "station identity armed for BSSID $BSSID" "$OUT/sta.log" 30; then
    ok "$cell: the station armed its filter for $BSSID, the address the AP's TAP carries"
  else
    bad "$cell: the station never armed $BSSID - see $OUT/sta.log (has ap_wpa2's kBssid moved?)"
    stop_both; return
  fi

  # The AP is the authenticator, so its log is the witness that matters: it
  # verified our msg2 MIC and our msg4 MIC with a PTK it derived itself.
  if wait_for "4-WAY HANDSHAKE COMPLETE" "$OUT/ap.log" 40; then
    ok "$cell: the AP completed the four-way (both MICs verified at the AUTHENTICATOR)"
  else
    bad "$cell: the four-way did not complete - see $OUT/ap.log"
    stop_both; return
  fi

  ping_cell "$cell" "station -> AP, encrypted" up
  # THE DOWNLINK, which is a different path and a different set of bugs: the
  # AP's TAP reader, its ARP flood under the GTK, and its per-station relay.
  # Phase 2b.8 closed on a bench run that no script reproduced; this is that
  # path under a graded cell.
  ping_cell "$cell" "AP -> station, encrypted" down

  stop_both

  # BOTH LEDGERS. Two independent accounts of the same frames - the ping
  # cannot tell "it worked" from "it worked by some other route", and neither
  # ledger alone can tell a decrypt from a plaintext fallback.
  local s_assoc s_enc s_plain s_mic s_nokey
  s_assoc=$(led 'associations'); s_enc=$(led 'encrypted rx')
  s_plain=$(led 'plaintext rx'); s_mic=$(led 'MIC failures')
  s_nokey=$(led 'no key for it')
  if [ "${s_assoc:-0}" = 1 ] && [ "${s_enc:-0}" -gt 0 ] 2>/dev/null &&
     [ "${s_plain:-1}" = 0 ] && [ "${s_mic:-1}" = 0 ] && [ "${s_nokey:-1}" = 0 ]; then
    ok "$cell: station ledger - one association, $s_enc encrypted frames in, no plaintext, no MIC failure"
  else
    bad "$cell: station ledger says associations=$s_assoc encrypted_rx=$s_enc plaintext_rx=$s_plain MIC=$s_mic no_key=$s_nokey (expected 1, >0, 0, 0, 0)"
  fi

  local a_toap a_fromhost a_tohost a_mic a_replay
  a_toap=$(apled 'to this AP'); a_fromhost=$(apled 'from host')
  a_tohost=$(apled 'to host'); a_mic=$(apled 'MIC failures')
  a_replay=$(apled 'replays rejected')
  # THE AIR WAS THE PATH, stated by the far end: the AP decrypted frames
  # addressed to itself and handed them to its own host stack. A local route
  # produces none of these, and neither does a station talking to something
  # else on the channel.
  if [ "${a_toap:-0}" -gt 0 ] 2>/dev/null &&
     [ "${a_tohost:-0}" -gt 0 ] 2>/dev/null &&
     [ "${a_fromhost:-0}" -gt 0 ] 2>/dev/null &&
     [ "${a_mic:-1}" = 0 ] && [ "${a_replay:-1}" = 0 ]; then
    ok "$cell: AP ledger - $a_toap frames decrypted for itself, $a_tohost up to its host, $a_fromhost back down, no MIC failure"
  else
    bad "$cell: AP ledger says to_this_AP=$a_toap tap_to_host=$a_tohost tap_from_host=$a_fromhost MIC=$a_mic replays=$a_replay (expected >0, >0, >0, 0, 0)"
  fi
}

# --- cell: the falsifier ---------------------------------------------------
#
# EVERY CELL ABOVE COULD PASS WITHOUT AN RF LINK, and nothing in them proves
# otherwise. Both TAPs are ordinary kernel interfaces on one host; the netns
# and the `ip route get` assertion are what stand between this harness and a
# ping that never leaves the machine, and an assertion nobody has watched
# fail is a claim, not a check.
#
# So: put the two radios on DIFFERENT CHANNELS and run the same plumbing. The
# station's TAP comes up, its address is configured, the route still leaves
# through it - and the ping must be lost, every packet. If it is not, the
# 0% loss the other cells report says nothing about the air.
#
# The station's scan list is PINNED to its own channel on purpose. Left to
# sweep it might find the AP, and then "no association" would be a statement
# about the scanner rather than about the radio link - an ambiguous failure
# in the one cell whose job is to be unambiguous.
cell_airgap() {
  local freq5 freq
  freq=$(chan_freq "$CH") || { bad "airgap: channel '$CH' is not one this harness can map"; return; }
  freq5=$(chan_freq "$CH5") || { bad "airgap: channel '$CH5' is not one this harness can map"; return; }
  [ "$CH" != "$CH5" ] || { bad "airgap: CH and CH5 are both $CH - there is no gap to test"; return; }
  say "== airgap: AP on ch$CH5 ($freq5 MHz), station on ch$CH ($freq MHz) - the link MUST NOT form =="

  build_both || { bad "airgap: build"; return; }
  ns_up      || { bad "airgap: could not create netns $NS"; return; }
  ap_up "$CH5" $((AIRGAP_SECS + 30)) || {
    bad "airgap: the devourer AP did not come up"; stop_both; return; }
  ok "airgap: the AP is beaconing on ch$CH5"

  sta_up "$CH" "$AIRGAP_SECS" DEVOURER_STA_SCAN_CHANNELS="$CH" || {
    bad "airgap: sta_client did not start"; stop_both; return; }
  sta_tap_up || { stop_both; return; }
  ok "airgap: the station's TAP is up and the route STILL leaves through it"

  if wait_for "station identity armed for BSSID" "$OUT/sta.log" 25; then
    bad "airgap: the station associated with an AP on a different channel - one of the two is not tuned where it was told"
    stop_both; return
  fi
  ok "airgap: the station found nothing to join on ch$CH in 25s"

  ping -c 4 -W 2 -I "$STATAP" "$APIP" >"$OUT/airgap.ping" 2>&1
  if grep -q " 100% packet loss" "$OUT/airgap.ping"; then
    ok "airgap: the ping is lost, all four - so the 0% loss in the cells above is the RF link and not this host"
  else
    bad "airgap: $(grep -oE '[0-9.]+% packet loss' "$OUT/airgap.ping" | head -1) with the radios on different channels - the data plane is NOT going over the air, and every other cell here is void"
  fi
  stop_both
}

# --- cell: software CCMP cost at both ends ---------------------------------
#
# The same caveat the other two bench cells carry: a flood ping is ROUND-TRIP
# bound, so reply_pps is a LATENCY figure and not link throughput. Nothing in
# this tree measures the throughput of a station link. What this adds over
# tests/mt7612u_sta_onair.sh's bench is the FAR END's profile of the same
# frames - the station's transmit cost and the AP's receive cost are now two
# measurements of one direction.
cell_bench() {
  say "== bench: software CCMP cost at both ends (${BENCH_PAYLOAD}B, ${BENCH_SECS}s, ch$CH) =="
  case "$BENCH_PAYLOAD" in
    ''|*[!0-9]*) bad "bench: BENCH_PAYLOAD must be an integer"; return ;;
  esac
  [ "$BENCH_PAYLOAD" -ge 0 ] && [ "$BENCH_PAYLOAD" -le 1400 ] || {
    bad "bench: BENCH_PAYLOAD must be 0..1400 (avoid IP fragmentation)"; return; }
  build_both || { bad "bench: build"; return; }
  ns_up      || { bad "bench: could not create netns $NS"; return; }
  local run_secs=$(( SECS + BENCH_SECS ))
  ap_up "$CH" $((run_secs + 30)) DEVOURER_CCMP_PROFILE=1 || {
    bad "bench: the devourer AP did not come up"; stop_both; return; }
  sta_up "$CH" "$run_secs" DEVOURER_CCMP_PROFILE=1 || {
    bad "bench: sta_client did not start"; stop_both; return; }
  sta_tap_up || { stop_both; return; }
  wait_for "4-WAY HANDSHAKE COMPLETE" "$OUT/ap.log" 40 || {
    bad "bench: the four-way did not complete"; stop_both; return; }

  # PACED, NOT FLOODED, and the pacing is the finding rather than a
  # convenience. A flood offers ~100 packets/s and this link delivers about
  # twenty: see cell_flood below, which measures that ceiling and attributes
  # it. Timing a cipher inside a window where the link is collapsing measures
  # the collapse. BENCH_PPS is what it sustains.
  ping -c 1 -W 3 -I "$STATAP" "$APIP" >/dev/null 2>&1
  ping -q -i "$(awk -v p="$BENCH_PPS" 'BEGIN{printf "%.3f", 1/p}')" \
       -c $(( BENCH_PPS * BENCH_SECS )) -s "$BENCH_PAYLOAD" -W 2 \
       -I "$STATAP" "$APIP" >"$OUT/bench.ping" 2>&1 || true
  local tx rx loss
  tx=$(sed -n 's/^\([0-9][0-9]*\) packets transmitted.*/\1/p' "$OUT/bench.ping" | tail -1)
  rx=$(sed -n 's/.* \([0-9][0-9]*\) received.*/\1/p' "$OUT/bench.ping" | tail -1)
  loss=$(sed -n 's/.* \([0-9.][0-9.]*%\) packet loss.*/\1/p' "$OUT/bench.ping" | tail -1)
  tx=${tx:-0}; rx=${rx:-0}; loss=${loss:-unknown}
  # A MAJORITY, not zero, and the threshold is where it is for a reason that
  # is not a fudge. This link loses large frames: measured 27% at 10 pps of
  # 1400 B on ch6, against 0% for the same pair at one packet a second. The
  # loss is a property of this bench (a 2.4 GHz band with a dozen neighbours)
  # and of a data plane with no link-layer retransmission at either end -
  # neither harness arms SetAckResponder - so asserting zero here would grade
  # the room. What this bound catches is the case the figures below cannot
  # survive: a link that was collapsing while the cipher was being timed.
  if [ "${rx:-0}" -gt 0 ] && [ "$rx" -ge $(( tx / 2 )) ]; then
    ok "bench: $rx/$tx replies at ${BENCH_PPS} pps, loss=$loss"
  else
    bad "bench: only $rx/$tx replies (loss=$loss) at ${BENCH_PPS} pps - the cipher was timed while the link was failing"
  fi

  stop_both
  # AND IT STAYED ASSOCIATED. A per-frame cost averaged across a window that
  # contains a re-association is an average over two different links plus the
  # handshake between them.
  local rc; rc=$(led 'reconnects')
  if [ "${rc:-1}" = 0 ]; then
    ok "bench: one association throughout the measurement"
  else
    bad "bench: the station re-associated $rc time(s) during the measurement - the figures below average across a link that was falling over"
  fi
  local sprof aprof
  sprof=$(grep '"ev":"ccmp.profile"' "$OUT/sta.log" | tail -1)
  aprof=$(grep '"ev":"ccmp.profile"' "$OUT/ap.log" | tail -1)
  [ -n "$sprof" ] || { bad "bench: the station printed no ccmp.profile"; return; }
  [ -n "$aprof" ] || { bad "bench: the AP printed no ccmp.profile"; return; }
  # A PROFILE OF ZERO FRAMES IS NOT A MEASUREMENT. The first bench run of the
  # other harness printed ccmp_tx_ns_per_frame=0 over 3365 encrypted round
  # trips and passed, because replies say nothing about whether anything was
  # timed.
  local ok_counts=1 w
  for w in "$sprof" "$aprof"; do
    local t r
    t=$(printf '%s' "$w" | sed -n 's/.*"tx_frames":\([0-9][0-9]*\).*/\1/p')
    r=$(printf '%s' "$w" | sed -n 's/.*"rx_frames":\([0-9][0-9]*\).*/\1/p')
    [ "${t:-0}" -gt 0 ] 2>/dev/null && [ "${r:-0}" -gt 0 ] 2>/dev/null || ok_counts=0
  done
  if [ "$ok_counts" = 1 ]; then
    ok "bench: the cipher was timed at BOTH ends"
  else
    bad "bench: a profile counted zero frames - the ns/frame figures below are meaningless"
  fi
  python3 - "$sprof" "$aprof" "$BENCH_PAYLOAD" "$BENCH_SECS" "$tx" "$rx" "$CH" <<'PYEOF' || true
import json, sys
def ns(p, which):
    n = p[f"{which}_frames"]
    return p[f"{which}_ns"] / n if n else 0
s = json.loads(sys.argv[1]); a = json.loads(sys.argv[2])
row = {"ev":"devourer.d2d_ccmp_bench", "path":s["path"], "channel":int(sys.argv[7]),
       "ping_payload":int(sys.argv[3]), "seconds":int(sys.argv[4]),
       "requests":int(sys.argv[5]), "replies":int(sys.argv[6]),
       "reply_pps":int(sys.argv[6])/int(sys.argv[4]),
       "sta_ccmp_tx_ns_per_frame":ns(s,"tx"), "sta_ccmp_rx_ns_per_frame":ns(s,"rx"),
       "ap_ccmp_tx_ns_per_frame":ns(a,"tx"),  "ap_ccmp_rx_ns_per_frame":ns(a,"rx"),
       "sta_tx_frames":s["tx_frames"], "ap_rx_frames":a["rx_frames"]}
print(json.dumps(row, separators=(",",":")))
PYEOF
}

# --- cell: the ceiling, and where the frames above it go -------------------
#
# Offer ~100 packets/s and see what comes back. THIS CELL DOES NOT GRADE THE
# LOSS, because the loss is a property of this bench and this pair of
# adapters, and a number like that turns into a regression test for the room.
# What it grades is the BOOKKEEPING: every frame a host handed an endpoint is
# either on the air or counted as lost, by name, at the end it was lost.
#
# That is the check because that is what was broken. Under this exact load
# the AP's ledger read "TAP: from host=1437, dropped=0" beside "frames
# sent=232" and nothing else - 1205 frames taken from the host, aired
# nowhere, and every counter saying the data plane was clean. They went into
# a 128-frame queue cap with no counter and into send_packet refusals with no
# counter. A ledger that cannot fail this check is a ledger you cannot use to
# diagnose a link, which is the only reason it exists.
cell_flood() {
  say "== flood: the ceiling, and whether both ledgers account for it (${BENCH_PAYLOAD}B, ${BENCH_SECS}s, ch$CH) =="
  build_both || { bad "flood: build"; return; }
  ns_up      || { bad "flood: could not create netns $NS"; return; }
  local run_secs=$(( SECS + BENCH_SECS ))
  ap_up "$CH" $((run_secs + 30)) || { bad "flood: the AP did not come up"; stop_both; return; }
  sta_up "$CH" "$run_secs" || { bad "flood: sta_client did not start"; stop_both; return; }
  sta_tap_up || { stop_both; return; }
  wait_for "4-WAY HANDSHAKE COMPLETE" "$OUT/ap.log" 40 || {
    bad "flood: the four-way did not complete"; stop_both; return; }

  ping -c 1 -W 3 -I "$STATAP" "$APIP" >/dev/null 2>&1
  ping -f -q -s "$BENCH_PAYLOAD" -w "$BENCH_SECS" -I "$STATAP" "$APIP" \
      >"$OUT/flood.ping" 2>&1 || true
  local tx rx loss
  tx=$(sed -n 's/^\([0-9][0-9]*\) packets transmitted.*/\1/p' "$OUT/flood.ping" | tail -1)
  rx=$(sed -n 's/.* \([0-9][0-9]*\) received.*/\1/p' "$OUT/flood.ping" | tail -1)
  loss=$(sed -n 's/.* \([0-9.][0-9.]*%\) packet loss.*/\1/p' "$OUT/flood.ping" | tail -1)
  tx=${tx:-0}; rx=${rx:-0}; loss=${loss:-unknown}
  stop_both

  # THE STATION'S BOOKS. Everything its host handed it either went out
  # encrypted or was refused at a named counter: not connected / malformed /
  # not from our own address (g_tap_drop), the 128-frame queue cap, or the
  # device.
  local s_from s_tdrop s_enc s_qdrop s_sfail s_sum
  s_from=$(led 'from host'); s_tdrop=$(sed -n 's/.*TAP: to host=[0-9]*, from host=[0-9]*, dropped=\([0-9]*\).*/\1/p' "$OUT/sta.log" | tail -1)
  s_enc=$(sed -n 's/.*tx: encrypted=\([0-9]*\).*/\1/p' "$OUT/sta.log" | tail -1)
  s_qdrop=$(led 'queue dropped'); s_sfail=$(led 'send failed')
  s_sum=$(( ${s_enc:-0} + ${s_tdrop:-0} + ${s_qdrop:-0} + ${s_sfail:-0} ))
  if [ "${s_from:-0}" -gt 0 ] 2>/dev/null && [ "$s_sum" = "${s_from:-0}" ]; then
    ok "flood: the station's books balance - $s_from from its host = $s_enc aired + $s_tdrop refused + $s_qdrop queue-dropped + $s_sfail send-failed"
  else
    bad "flood: the station's books do NOT balance - $s_from from its host, but aired+refused+dropped+failed = $s_sum ($s_enc/$s_tdrop/$s_qdrop/$s_sfail). Frames are being lost at no counter."
  fi

  # THE AP'S BOOKS, the same identity from the other end.
  local a_from a_sent a_qdrop a_sfail a_sum
  a_from=$(sed -n 's/.*TAP: to host=[0-9]*, from host=\([0-9]*\).*/\1/p' "$OUT/ap.log" | tail -1)
  a_sent=$(apled 'frames sent'); a_qdrop=$(apled 'queue dropped')
  a_sfail=$(apled 'send failed')
  a_sum=$(( ${a_sent:-0} + ${a_qdrop:-0} + ${a_sfail:-0} ))
  # The AP AIRS MORE THAN ITS HOST GIVES IT - beacons are the chip's, but
  # auth, assoc, the four-way and the group flood all go through the same
  # queue - so this is a bound, not an equality: nothing may vanish.
  if [ "${a_from:-0}" -gt 0 ] 2>/dev/null && [ "$a_sum" -ge "${a_from:-0}" ]; then
    ok "flood: the AP's books account for all $a_from frames from its host - $a_sent aired, $a_qdrop queue-dropped, $a_sfail refused by the device"
  else
    bad "flood: the AP's books do NOT account for its host's traffic - $a_from in, aired+dropped+failed = $a_sum ($a_sent/$a_qdrop/$a_sfail). Frames are being lost at no counter."
  fi

  say "  offered $tx, delivered $rx ($loss) = $(awk -v r="$rx" -v s="$BENCH_SECS" 'BEGIN{printf "%.0f", r/s}') round trips/s."
  say "  THIS IS A CEILING, NOT A THROUGHPUT: a flood ping is round-trip bound,"
  say "  and nothing in this tree measures the throughput of a station link."
}

# Expected check counts, so the advertised score is machine-enforced rather
# than counted by eye - a future edit that drops an ok()/bad() pair would
# otherwise run one check fewer, exit 0, and still be read as "N/N".
case "$CELLS" in
  wpa2)    cell_link wpa2 "$CH";           want=8 ;;
  fiveghz) cell_link fiveghz "$CH5";       want=8 ;;
  airgap)  cell_airgap;                    want=4 ;;
  bench)   cell_bench;                     want=3 ;;
  flood)   cell_flood;                     want=2 ;;
  all)     cell_link wpa2 "$CH"; cleanup
           cell_link fiveghz "$CH5"; cleanup
           cell_airgap;                    want=20 ;;
  *)       echo "usage: $0 [wpa2|fiveghz|airgap|bench|flood|all]"; exit 2 ;;
esac

say ""
say "=== $pass passed, $fail failed   (logs: $OUT) ==="
say "=== devourer station ($sta_vid:$sta_pid) <-> devourer AP ($ap_vid:$ap_pid), no kernel 802.11 ==="
if [ "$fail" -eq 0 ] && [ "$pass" -ne "$want" ]; then
  say "=== HARNESS ERROR: $pass checks passed, none failed, but $want were expected ==="
  say "=== a check went missing - do NOT read this as $want/$want ==="
  exit 2
fi
exit $(( fail > 0 ))
