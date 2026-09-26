#!/usr/bin/env bash
# mt7612u_sta_onair.sh — devourer as a STATION, end to end, on hardware.
#
# The mirror of tests/mt7612u_ap_onair.sh. There the MT7612U serves a BSS and
# a kernel-driven adapter joins it; here the MT7612U JOINS one and the
# kernel-driven adapter serves it, running hostapd.
#
# THE AP IS AN INDEPENDENT IMPLEMENTATION ON INDEPENDENT SILICON, and that is
# the point rather than a convenience. docs/mt7612u-ap-mode.md states its own
# weakness plainly - its station was the same silicon, so it was not an
# independent-generation witness - and docs/station-mode-plan.md's Phase 5
# says the station work must not repeat that. hostapd on an RTL8812AU
# (rtw88_8812au) is a different vendor, a different driver and a different
# codebase, and it has already earned its keep: on the first WPA2 run it
# refused our group rekey and threw the station off the BSS, a defect nothing
# in this tree could have found because this project's own AP never sends a
# group message 1.
#
# THE AP LIVES IN A NETWORK NAMESPACE, and that is load-bearing. Both radios
# are on one host, so with both interfaces in the root namespace and both
# addresses in one subnet the kernel routes between them LOCALLY and the ping
# never touches the air - a data-plane cell that passes with the antennas
# unplugged. The PHY is moved with `iw phy <phy> set netns`, because a
# cfg80211 interface cannot be moved with `ip link set netns`, and every cell
# ASSERTS `ip route get` before it measures anything.
#
# Four cells, each with its own witness:
#
#   open       hostapd open       associate -> ARP/ICMP over an open link
#   wpa2       hostapd WPA2-PSK   4-way -> group AND pairwise rekey -> ping
#   reconnect  hostapd stopped    beacon loss noticed -> AP back -> re-joined
#   bench      hostapd WPA2-PSK   software CCMP cost under a flood ping
#
#   sudo tests/mt7612u_sta_onair.sh
#   sudo STA_SYSFS=1-1 AP_SYSFS=8-1 CH=6 tests/mt7612u_sta_onair.sh wpa2
#   sudo FW_DIR=/lib/firmware/mediatek tests/mt7612u_sta_onair.sh all
#
# Env: STA_SYSFS (the MT7612U devourer claims), AP_SYSFS (the adapter hostapd
# drives), CH, PSK, FW_DIR, SECS, NS, TAP, REKEY_S, BENCH_SECS, BENCH_PAYLOAD.
# Cells: open|wpa2|reconnect|bench|all (all is the three acceptance cells).

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CELLS="${1:-all}"

STA_SYSFS="${STA_SYSFS:-1-1}"
AP_SYSFS="${AP_SYSFS:-8-1}"
CH="${CH:-6}"
case "$CH" in
  ''|*[!0-9]*) echo "CH must be a channel number"; exit 2 ;;
esac
# The same mapping the AP harness uses, and for the same reason: a wrong
# frequency turns every check into a silent "not reachable".
if   [ "$CH" -ge 1 ]  && [ "$CH" -le 13 ]; then FREQ=$(( 2407 + CH * 5 ))
elif [ "$CH" = 14 ];                       then FREQ=2484
elif [ "$CH" -ge 34 ] && [ "$CH" -le 196 ]; then FREQ=$(( 5000 + CH * 5 ))
else echo "CH=$CH is not a 2.4 or 5 GHz channel this harness can map"; exit 2
fi
PSK="${PSK:-devourer123}"
SSID="${SSID:-devourerSTA}"
FW_DIR="${FW_DIR:-/lib/firmware/mediatek}"
# THE STATION'S BUDGET IS MEASURED FROM PROCESS START, NOT FROM ASSOCIATION,
# and the waits in front of the measurement are not short: ~10 s of firmware
# load, up to 20 s for the TAP, up to 25 s for the AP to report the
# association. At 45 s the open cell's ping straddled the station's own exit
# and reported 66.7% loss on a link that was working - once, in one run of
# three, which is the worst kind of number to have to explain later.
SECS="${SECS:-90}"
NS="${NS:-staonair}"
TAP="${TAP:-dvsta0}"
# hostapd's group rekey interval. The wpa2 cell needs at least one to happen
# inside its run, because the rekey is the half that was broken and a cell
# that never provokes it proves only the four-way.
REKEY_S="${REKEY_S:-20}"
# And the PAIRWISE key's interval. A PTK rekey runs the four-way again with
# the association already up, so nothing hung off "we just connected" sees it
# - which is exactly how the station's replay windows came to be reset in the
# wrong place. hostapd does not do this unless asked; no default
# configuration anywhere would have exercised it.
PTK_REKEY_S="${PTK_REKEY_S:-25}"
BENCH_SECS="${BENCH_SECS:-15}"
BENCH_PAYLOAD="${BENCH_PAYLOAD:-1400}"
APIP=192.168.98.1
STAIP=192.168.98.2
OUT="${OUT:-/tmp/mt7612u-sta-onair}"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
mkdir -p "$OUT"
pass=0; fail=0
say()  { printf '%s\n' "$*"; }
ok()   { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad()  { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

# PIDs this script started, so cleanup kills those and nothing else. A name
# kill would reach a concurrent run of this same test, and `pkill -x hostapd`
# would drop any AP the operator is running for other reasons.
KIDS=""
reap() {
  local pid
  for pid in $KIDS; do kill "$pid" 2>/dev/null; done
  KIDS=""
}

# EXACT name match: `grep "^$NS"` also matched any namespace whose name
# merely STARTS with $NS.
ns_exists() { ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "$NS"; }
NS_OURS=no
TAP_OURS=no

cleanup() {
  reap
  [ -f "$OUT/hostapd.pid" ] && { kill "$(cat "$OUT/hostapd.pid")" 2>/dev/null
                                 rm -f "$OUT/hostapd.pid"; }
  [ "$TAP_OURS" = yes ] && ip addr flush dev "$TAP" 2>/dev/null
  # THE PHY MUST COME BACK BEFORE THE NAMESPACE GOES. A script that dies with
  # the adapter still in its namespace leaves the operator an adapter that has
  # simply vanished - no netdev, nothing in `iw dev`, and no hint where it
  # went. Moving it back is the single most important thing this trap does.
  #
  # AND `ip netns del` ON A NAMESPACE THAT STILL HOLDS THE PHY DESTROYS IT.
  # Measured, by doing exactly that by hand: the USB device stayed bound and
  # enumerated, /sys/class/ieee80211 lost the phy entirely, and only a bus
  # re-enumeration brought it back. So the delete is CONDITIONAL on the move
  # having worked, and if it did not, this says so and leaves the namespace
  # alone rather than turning a recoverable mess into a lost adapter.
  if [ "$NS_OURS" = yes ] && ns_exists; then
    if [ -n "${AP_PHY:-}" ]; then
      ip netns exec "$NS" iw phy "$AP_PHY" set netns 1 2>/dev/null
      sleep 1
    fi
    if ip netns exec "$NS" ls /sys/class/ieee80211/ 2>/dev/null | grep -q .; then
      say "WARNING: a phy is still in netns $NS - NOT deleting it, because"
      say "         that would destroy the phy. Recover with:"
      say "           sudo ip netns exec $NS iw phy <phy> set netns 1"
      say "           sudo ip netns del $NS"
    else
      ip netns del "$NS" 2>/dev/null
      NS_OURS=no
    fi
  fi
  # The TAP is sta_client's and dies with it; this only catches a leak - and
  # only of a name this run was the one to claim (see the preflight).
  [ "$TAP_OURS" = yes ] && ip link del "$TAP" 2>/dev/null
  return 0
}
trap cleanup EXIT
# AND IT MUST STOP: with INT/TERM on the EXIT trap, bash runs cleanup and then
# CARRIES ON into the next cell (tests/sta_d2d_onair.sh found and fixed this).
trap 'cleanup; exit 130' INT TERM

# --- preflight -------------------------------------------------------------

# A phy left in our namespace by a run that died. Recover it before doing
# anything else, or the AP interface below is simply "not found".
#
# ONLY A NAMESPACE THAT LOOKS LIKE OURS. A leftover of this harness holds a
# wireless phy and its netdev, nothing else; a namespace with any other
# interface in it (a veth, a bridge) is the operator's, and moving its phys
# out and deleting it would destroy their setup. Refused instead.
if ns_exists; then
  foreign=$(ip netns exec "$NS" sh -c 'for d in /sys/class/net/*; do
              n=${d##*/}; [ "$n" = lo ] && continue
              [ -e "$d/phy80211" ] || echo "$n"; done' 2>/dev/null)
  if [ -n "$foreign" ]; then
    echo "netns $NS exists and holds non-wireless interfaces ($foreign) -"
    echo "not a leftover of this harness. Refusing to use or delete it (set NS=)."
    exit 2
  fi
  NS_OURS=yes
  say "a previous run left the namespace $NS - recovering"
  for p in $(ip netns exec "$NS" ls /sys/class/ieee80211/ 2>/dev/null); do
    ip netns exec "$NS" iw phy "$p" set netns 1 2>/dev/null
  done
  sleep 2
  # Only once it is empty - see cleanup() for what deleting it otherwise does.
  if ! ip netns exec "$NS" ls /sys/class/ieee80211/ 2>/dev/null | grep -q .; then
    ip netns del "$NS" 2>/dev/null && NS_OURS=no
  fi
fi

rfkill unblock wlan 2>/dev/null || true

AP_IF=$(ls "/sys/bus/usb/devices/$AP_SYSFS:1.0/net/" 2>/dev/null | head -1)
if [ -z "$AP_IF" ]; then
  echo "$AP_SYSFS:1.0" > /sys/bus/usb/drivers_probe 2>/dev/null
  sleep 4
  AP_IF=$(ls "/sys/bus/usb/devices/$AP_SYSFS:1.0/net/" 2>/dev/null | head -1)
fi
if [ -z "$AP_IF" ]; then
  # A re-probe is not enough once the phy has been destroyed (see cleanup()),
  # or after devourer has held the adapter - the libusb claim leaves the
  # interface on usbfs and only a re-enumeration produces a netdev.
  #
  # Confirmed against the device first: this runs as root and writes to a
  # path the CALLER supplied, so a stale AP_SYSFS would otherwise re-enumerate
  # whatever else is plugged there - a keyboard, a disk. Refuse a hub, refuse
  # the root device, and refuse the station's own path.
  ap_cls=$(cat "/sys/bus/usb/devices/$AP_SYSFS/bDeviceClass" 2>/dev/null)
  ap_vid=$(cat "/sys/bus/usb/devices/$AP_SYSFS/idVendor" 2>/dev/null)
  if [ -z "$ap_vid" ] || [ "$AP_SYSFS" = "$STA_SYSFS" ] || [ "$ap_cls" = "09" ]; then
    echo "refusing to re-enumerate $AP_SYSFS (vid='$ap_vid' class='$ap_cls')"
    echo "- it is not a plausible AP device. Check AP_SYSFS."
    exit 2
  fi
  say "no netdev at $AP_SYSFS - re-enumerating it ($ap_vid)"
  echo 0 > "/sys/bus/usb/devices/$AP_SYSFS/authorized" 2>/dev/null
  sleep 3
  echo 1 > "/sys/bus/usb/devices/$AP_SYSFS/authorized" 2>/dev/null
  sleep 10
  AP_IF=$(ls "/sys/bus/usb/devices/$AP_SYSFS:1.0/net/" 2>/dev/null | head -1)
fi
[ -n "$AP_IF" ] || {
  echo "no netdev at $AP_SYSFS - hostapd needs a kernel-driven adapter there"
  echo "(devourer's libusb claim leaves an adapter on usbfs; re-enumerate it)"
  exit 2; }

AP_PHY=$(basename "$(readlink -f "/sys/class/net/$AP_IF/phy80211")" 2>/dev/null)
[ -n "$AP_PHY" ] || { echo "cannot find the phy behind $AP_IF"; exit 2; }
AP_MAC=$(cat "/sys/class/net/$AP_IF/address" 2>/dev/null)

# hostapd needs AP mode. Saying so here beats a cryptic nl80211 failure.
if ! iw phy "$AP_PHY" info 2>/dev/null |
     sed -n '/Supported interface modes/,/^\t[A-Za-z]/p' | grep -q '\* AP$'; then
  echo "$AP_IF ($AP_PHY) does not support AP mode - pick another AP_SYSFS"
  exit 2
fi

# The MT7612U must NOT have a kernel driver bound: devourer claims it over
# libusb. Checked by VID:PID, because this runs as root and a stale STA_SYSFS
# would otherwise have us unbind whatever else is plugged there.
sta_vid=$(cat "/sys/bus/usb/devices/$STA_SYSFS/idVendor" 2>/dev/null)
sta_pid=$(cat "/sys/bus/usb/devices/$STA_SYSFS/idProduct" 2>/dev/null)
[ "$sta_vid" = "0e8d" ] && [ "$sta_pid" = "7612" ] || {
  echo "STA_SYSFS=$STA_SYSFS is not an MT7612U (found '$sta_vid:$sta_pid')"
  exit 2; }

# NetworkManager will happily autoconnect the AP adapter to a real network in
# the middle of a cell, and will manage the TAP the station creates. Both are
# ours for the duration. Restored at the end - this is host state.
NM_AP=""; NM_TAP=""
if command -v nmcli >/dev/null 2>&1; then
  case "$(nmcli -t -f DEVICE,STATE device 2>/dev/null | grep "^$AP_IF:")" in
    "$AP_IF:unmanaged") : ;;
    "$AP_IF:"*) nmcli device set "$AP_IF" managed no >/dev/null 2>&1 &&
                NM_AP=yes ;;
  esac
fi

# THE STATION'S TAP NAME MUST BE FREE - the same rule, and the same reason,
# as tests/sta_d2d_onair.sh: sta_client creates it per cell and cleanup
# deletes it, so a pre-existing interface of that name would be addressed,
# used and then destroyed. Only after this check is the name ours.
if [ -e "/sys/class/net/$TAP" ]; then
  echo "TAP=$TAP already exists - refusing to use or delete it (set TAP=<other name>)"
  exit 2
fi
TAP_OURS=yes

say "station $STA_SYSFS (MT7612U, devourer)   AP $AP_SYSFS ($AP_IF $AP_MAC, $AP_PHY)"
say "ch$CH ($FREQ MHz)  ssid '$SSID'  ns '$NS'  tap '$TAP'"

# --- build -----------------------------------------------------------------

build_sta() {
  [ -f "$BUILD/libdevourer.a" ] || {
    say "no $BUILD/libdevourer.a - build the library first"; return 1; }
  g++ -std=c++20 -O2 -I"$ROOT/src" -I"$ROOT/tests" -I"$ROOT/examples/common" \
      "$ROOT/tests/sta_client.cpp" "$ROOT/examples/common/env_config.cpp" \
      "$ROOT/examples/common/usb_select.cpp" "$BUILD/libdevourer.a" \
      $(pkg-config --cflags --libs libusb-1.0) -lcrypto -lpthread \
      -o "$OUT/sta_client" || return 1
}

staenv() {
  set -- DEVOURER_VID=0x0e8d DEVOURER_PID=0x7612 \
         DEVOURER_USB_BUS="${STA_SYSFS%%-*}" \
         DEVOURER_USB_PORT="${STA_SYSFS#*-}" \
         DEVOURER_CHANNEL="$CH" DEVOURER_TX_WITH_RX=thread \
         DEVOURER_MT7612U_FW_DIR="$FW_DIR" \
         DEVOURER_STA_SSID="$SSID" DEVOURER_STA_TAP="$TAP" "$@"
  printf '%s\n' "$@"
}

# --- the AP ----------------------------------------------------------------

ap_conf() {   # $1 = "open" | "wpa2"
  {
    printf 'interface=%s\ndriver=nl80211\nssid=%s\n' "$AP_IF" "$SSID"
    if [ "$CH" -gt 14 ]; then printf 'hw_mode=a\n'; else printf 'hw_mode=g\n'; fi
    printf 'channel=%s\nieee80211n=1\nauth_algs=1\nwmm_enabled=1\n' "$CH"
    if [ "$1" = wpa2 ]; then
      printf 'wpa=2\nwpa_passphrase=%s\nwpa_key_mgmt=WPA-PSK\n' "$PSK"
      printf 'rsn_pairwise=CCMP\nwpa_group_rekey=%s\n' "$REKEY_S"
      [ "${PTK_REKEY_S:-0}" -gt 0 ] 2>/dev/null &&
        printf 'wpa_ptk_rekey=%s\n' "$PTK_REKEY_S"
    fi
  } > "$OUT/hostapd.conf"
}

ns_up() {
  # Ours by now if it exists (preflight refused anything else, and cleanup
  # removes our own between cells), so re-entry is a no-op.
  if ns_exists; then [ "$NS_OURS" = yes ] && return 0; return 1; fi
  ip netns add "$NS" || return 1
  NS_OURS=yes
  iw phy "$AP_PHY" set netns name "$NS" || return 1
  sleep 2
  ip netns exec "$NS" ip link set "$AP_IF" up || return 1
}

ap_up() {   # $1 = "open" | "wpa2"
  ap_conf "$1"
  rm -f "$OUT/hostapd.log" "$OUT/hostapd.pid"
  ip netns exec "$NS" hostapd -B -P "$OUT/hostapd.pid" \
      -f "$OUT/hostapd.log" "$OUT/hostapd.conf" >/dev/null 2>&1 || return 1
  local i
  for i in $(seq 1 20); do
    grep -q "AP-ENABLED" "$OUT/hostapd.log" 2>/dev/null && break
    sleep 1
  done
  grep -q "AP-ENABLED" "$OUT/hostapd.log" 2>/dev/null || return 1
  ip netns exec "$NS" ip addr add "$APIP/24" dev "$AP_IF" 2>/dev/null
  return 0
}

ap_down() {
  [ -f "$OUT/hostapd.pid" ] || return 0
  kill "$(cat "$OUT/hostapd.pid")" 2>/dev/null
  rm -f "$OUT/hostapd.pid"
  sleep 2
}

# --- the station -----------------------------------------------------------

sta_up() {   # $1 = seconds, $2.. = extra env
  local secs="$1"; shift
  rm -f "$OUT/sta.log"
  # An ARRAY, not `env $(staenv ...)`: the unquoted form word-split every
  # NAME=value, so an SSID or FW_DIR with a space became stray arguments.
  local -a e
  mapfile -t e < <(staenv "$@")
  env "${e[@]}" timeout $((secs + 20)) "$OUT/sta_client" "$secs" \
      >"$OUT/sta.log" 2>&1 &
  STA_PID=$!
  KIDS="$KIDS $STA_PID"
  local i
  for i in $(seq 1 25); do
    grep -q "sta_client up:" "$OUT/sta.log" 2>/dev/null && return 0
    kill -0 "$STA_PID" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}

# Bring the TAP up and prove the route leaves through it.
#
# WITHOUT THE ROUTE ASSERTION THIS WHOLE HARNESS IS THEATRE. Both radios are
# on one host; if the AP's address is reachable through anything but the TAP,
# the ping below measures the kernel's loopback and passes with the antennas
# unplugged.
tap_up() {
  local i
  for i in $(seq 1 20); do
    [ -d "/sys/class/net/$TAP" ] && break
    sleep 1
  done
  [ -d "/sys/class/net/$TAP" ] || { bad "the station created no TAP device"; return 1; }
  if command -v nmcli >/dev/null 2>&1; then
    nmcli device set "$TAP" managed no >/dev/null 2>&1 && NM_TAP=yes
  fi
  ip link set "$TAP" up 2>/dev/null
  ip addr flush dev "$TAP" 2>/dev/null
  ip addr add "$STAIP/24" dev "$TAP" 2>/dev/null
  sleep 1
  local route; route=$(ip route get "$APIP" 2>/dev/null)
  case "$route" in
    *"dev $TAP"*) return 0 ;;
    *) bad "the route to $APIP does not leave through $TAP - a ping would not touch the air ($route)"
       return 1 ;;
  esac
}

# The station's TAP carries the RADIO's MAC (sta_client sets it), so the AP's
# view of "which station" is the same address on both sides. Confirming it
# here turns a silent mismatch into a named failure.
sta_mac() { cat "/sys/class/net/$TAP/address" 2>/dev/null; }

wait_for() {   # $1 = pattern, $2 = file, $3 = seconds
  local i
  for i in $(seq 1 "$3"); do
    grep -q "$1" "$2" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

# "The AP associated US." MAC-MATCHED, not a substring: a bare
# `AP-STA-CONNECTED` is satisfied by any station on the channel, and - more
# to the point here - by OUR station transmitting from an address that is not
# the one the TAP carries, which is precisely the failure sta_client's
# SIOCSIFHWADDR exists to prevent. This helper existed and nothing called it;
# the weaker substring did the work.
wait_for_us() {   # $1 = seconds
  wait_for "AP-STA-CONNECTED $(sta_mac)" "$OUT/hostapd.log" "$1"
}

# One field out of the station's exit ledger, which is printed at every exit.
led() { sed -n "s/.*$1=\\([0-9][0-9]*\\).*/\\1/p" "$OUT/sta.log" | tail -1; }

# "Is the station still running?" A cell that measures a data plane after its
# own binary has exited reports PACKET LOSS, which is the most misleading
# answer available - it names the link, and the link was fine.
sta_alive() { kill -0 "$STA_PID" 2>/dev/null; }

# The ping, with that distinction built in. $1 = cell name, $2 = label.
ping_cell() {
  local cell="$1"
  if ! sta_alive; then
    bad "$cell: the station exited before the data plane could be measured - raise SECS (currently $SECS)"
    return 1
  fi
  ping -c 1 -W 3 -I "$TAP" "$APIP" >/dev/null 2>&1          # warm ARP
  ping -c 6 -W 1 -I "$TAP" "$APIP" 2>&1 | tee "$OUT/$cell.ping" >/dev/null
  if ! sta_alive; then
    bad "$cell: the station exited DURING the data-plane measurement - raise SECS (currently $SECS)"
    return 1
  fi
  if grep -q " 0% packet loss" "$OUT/$cell.ping"; then
    ok "$cell: $2 ($(grep -oE 'rtt [^ ]+ = [0-9./]+' "$OUT/$cell.ping" | head -1))"
    return 0
  fi
  bad "$cell: $2 lost packets ($(grep -oE '[0-9.]+% packet loss' "$OUT/$cell.ping" | head -1))"
  return 1
}

# Wait for the station to exit, so its ledger is complete before anything
# reads it. NOT a check: it used to also grep for a line report() prints
# unconditionally, and every caller discarded the result - a gate that could
# not fail and contributed no signal. The ledger assertions that follow it
# are the check, and they fail on their own when the ledger is missing.
sta_wait() { wait "$STA_PID" 2>/dev/null; }

# --- cell: open network ----------------------------------------------------
cell_open() {
  say "== open network (hostapd, no encryption) =="
  build_sta || { bad "open: build"; return; }
  ns_up || { bad "open: could not move $AP_PHY into $NS"; return; }
  ap_up open || { bad "open: hostapd did not come up (see $OUT/hostapd.log)"; return; }
  ok "open: AP on air"

  sta_up "$SECS" DEVOURER_STA_PSK= || { bad "open: sta_client did not start (see $OUT/sta.log)"; ap_down; return; }
  tap_up || { kill "$STA_PID" 2>/dev/null; ap_down; return; }
  ok "open: TAP up and the route leaves through it"

  if wait_for_us 25; then
    ok "open: the AP associated us ($(sta_mac))"
  else
    bad "open: the AP never associated $(sta_mac) - see $OUT/sta.log's refusal counters"
    kill "$STA_PID" 2>/dev/null; ap_down; return
  fi

  ping_cell open "data plane"

  kill "$STA_PID" 2>/dev/null
  sta_wait
  # The ledger, not the ping: a link that carried traffic AND logged crypto
  # errors is not the same result, and the ping cannot tell them apart.
  if [ "$(led 'plaintext rx')" -gt 0 ] 2>/dev/null &&
     [ "$(led 'encrypted rx')" = 0 ]; then
    ok "open: the station carried plaintext and never decrypted anything"
  else
    bad "open: ledger disagrees (plaintext rx=$(led 'plaintext rx'), encrypted rx=$(led 'encrypted rx'))"
  fi
  ap_down
}

# --- cell: WPA2-PSK --------------------------------------------------------
cell_wpa2() {
  say "== WPA2-PSK (hostapd, CCMP, group rekey every ${REKEY_S}s) =="
  build_sta || { bad "wpa2: build"; return; }
  ns_up || { bad "wpa2: could not move $AP_PHY into $NS"; return; }
  ap_up wpa2 || { bad "wpa2: hostapd did not come up"; return; }
  ok "wpa2: AP on air"

  # SECS covers the bring-up and the waits (see the note there); the rekey
  # intervals are on top of it, because this cell measures the half that was
  # broken and not only the four-way.
  local secs=$(( SECS + REKEY_S * 2 + PTK_REKEY_S ))
  sta_up "$secs" DEVOURER_STA_PSK="$PSK" || { bad "wpa2: sta_client did not start"; ap_down; return; }
  tap_up || { kill "$STA_PID" 2>/dev/null; ap_down; return; }

  if wait_for "EAPOL-4WAY-HS-COMPLETED" "$OUT/hostapd.log" 30; then
    ok "wpa2: the AP completed the four-way (MIC verified at the AUTHENTICATOR)"
  else
    bad "wpa2: the four-way did not complete - see $OUT/hostapd.log"
    kill "$STA_PID" 2>/dev/null; ap_down; return
  fi

  ping_cell wpa2 "encrypted data plane"

  # THE GROUP REKEY. This is the cell that earns its place: the four-way
  # passed on the first on-air run and the rekey did not, and the AP threw us
  # off the BSS for it. A station that cannot answer one is dropped by every
  # AP that performs them.
  if wait_for "group key handshake completed" "$OUT/hostapd.log" $(( REKEY_S + 25 )); then
    ok "wpa2: the AP completed a group rekey"
  else
    bad "wpa2: no group rekey completed in $(( REKEY_S + 25 ))s - grep 'group key handshake' $OUT/hostapd.log"
  fi

  # AND THE PAIRWISE REKEY, which is a different handshake with a different
  # failure. It runs the four-way again with the association already up, so
  # the answer has to go out under the OLD key - the authenticator does not
  # switch its own until it has accepted message 4. A station that replies
  # under the new one sends a frame the AP cannot read.
  #
  # Found by review, not by the bench: no default hostapd configuration
  # anywhere performs this, so `wpa_ptk_rekey` has to be asked for.
  #
  # COUNTED, NOT MATCHED. hostapd logs "pairwise key handshake completed" for
  # the INITIAL four-way too, so a `wait_for` on that string returns before
  # any rekey has happened - a check that cannot fail, which is the category
  # this branch keeps finding. A rekey is the SECOND one and later.
  local i pk=0
  for i in $(seq 1 $(( PTK_REKEY_S + 25 ))); do
    pk=$(grep -c "pairwise key handshake completed" "$OUT/hostapd.log" 2>/dev/null)
    [ "${pk:-0}" -ge 2 ] && break
    sleep 1
  done
  if [ "${pk:-0}" -ge 2 ]; then
    ok "wpa2: the AP completed a PAIRWISE rekey ($pk handshakes, the first being the association's)"
  else
    bad "wpa2: no pairwise rekey in $(( PTK_REKEY_S + 25 ))s (only $pk pairwise handshake(s), i.e. the association's alone)"
  fi

  # And it must not have cost us the association. "It reconnected" is not the
  # same result as "it stayed up", and only the ledger can tell them apart.
  sta_wait
  # THE LEDGER, not the AP's log. "It reconnected" is not the same result as
  # "it stayed up", and only this side can tell them apart - the AP sees a
  # fresh association either way.
  local assoc rekey mic ptk
  assoc=$(led 'associations'); rekey=$(led 'answered'); mic=$(led 'MIC failures')
  ptk=$(led 'PTK')
  # MIC FAILURES <= PAIRWISE REKEYS, not zero, and that bound is the theory
  # rather than a fudge. A pairwise rekey has a one-round-trip window in
  # which the AP is still transmitting under the old key and this side has
  # already installed the new one (802.11-2016 12.7.6.5), so at most one data
  # frame per rekey can fail its MIC. Measured: 1 across 6 rekeys, with the
  # ping at 0% loss. Asserting zero here would either fail on the protocol or
  # force a grace-period cache that costs a second replay window to save one
  # frame; asserting the bound catches a real MIC problem and tolerates this
  # one. See the note at ccmp_decrypt's caller in tests/sta_client.cpp.
  if [ "${assoc:-0}" = 1 ] && [ "${rekey:-0}" -gt 0 ] 2>/dev/null &&
     [ "${ptk:-0}" -ge 2 ] 2>/dev/null &&
     [ "${mic:-999}" -le "${ptk:-0}" ] 2>/dev/null; then
    ok "wpa2: one association throughout, $rekey rekey(s) answered, $ptk pairwise keys, $mic MIC failure(s) (<= $ptk, the rekey switchover window)"
  else
    bad "wpa2: ledger says associations=$assoc rekeys_answered=$rekey PTK_installs=$ptk MIC_failures=$mic (expected 1, >0, >=2, and MIC <= PTK)"
  fi
  ap_down
}

# --- cell: reconnect -------------------------------------------------------
#
# Phase 3 carried this forward explicitly: StationSm notices beacon loss and
# FAILS, and does not re-join, because when to retry is the integrator's
# decision. The policy lives in tests/sta_client.cpp and this is what reads
# it on air.
cell_reconnect() {
  say "== reconnect (AP goes away and comes back) =="
  build_sta || { bad "reconnect: build"; return; }
  ns_up || { bad "reconnect: could not move $AP_PHY into $NS"; return; }
  PTK_REKEY_S=0 ap_up wpa2 ||
    { bad "reconnect: hostapd did not come up"; return; }

  # Its own budget has to cover TWO associations plus an eight-second gap, so
  # it is longer than SECS rather than a fixed 70 - see the note there.
  sta_up $(( SECS + 40 )) DEVOURER_STA_PSK="$PSK" ||
    { bad "reconnect: sta_client did not start"; ap_down; return; }
  tap_up || { kill "$STA_PID" 2>/dev/null; ap_down; return; }
  ok "reconnect: TAP up and the route leaves through it"
  if wait_for "EAPOL-4WAY-HS-COMPLETED" "$OUT/hostapd.log" 30; then
    ok "reconnect: the link came up"
  else
    bad "reconnect: the link never came up"
    kill "$STA_PID" 2>/dev/null; ap_down; return
  fi
  ping -c 3 -W 1 -I "$TAP" "$APIP" >/dev/null 2>&1

  # THE AP DISAPPEARS. Not a deauth - the beacon simply stops, which is what
  # an AP losing power looks like and is the case StationSm's beacon
  # supervision exists for. A deauth would exercise a different path (and one
  # that needs no supervision at all).
  say "  stopping the AP for 8s"
  ap_down
  sleep 8

  # It must come back on the SAME BSSID, or the station is choosing a
  # different BSS and "it reconnected" would mean something else.
  rm -f "$OUT/hostapd.log"
  PTK_REKEY_S=0 ap_up wpa2 ||
    { bad "reconnect: the AP did not come back"; kill "$STA_PID" 2>/dev/null; return; }
  say "  AP back"

  if wait_for "EAPOL-4WAY-HS-COMPLETED" "$OUT/hostapd.log" 30; then
    ok "reconnect: the station re-associated and re-keyed by itself"
  else
    bad "reconnect: the station did not come back within 30s"
    kill "$STA_PID" 2>/dev/null; return
  fi

  # A re-association that carries no traffic is not a recovery. The keys are
  # new, both PN spaces restarted, and the replay windows had to restart with
  # them - a station that kept the old window rejects the new AP's first
  # frames as ancient replays and reports itself keyed while carrying nothing.
  ping_cell reconnect "the data plane came back"

  kill "$STA_PID" 2>/dev/null
  sta_wait
  local rc assoc
  rc=$(led 'reconnects'); assoc=$(led 'associations')
  if [ "${rc:-0}" -ge 1 ] 2>/dev/null && [ "${assoc:-0}" -ge 2 ] 2>/dev/null; then
    ok "reconnect: the station counted it (reconnects=$rc, associations=$assoc)"
  else
    bad "reconnect: ledger says reconnects=$rc associations=$assoc (expected >=1, >=2)"
  fi
  ap_down
}

# --- cell: software CCMP cost ----------------------------------------------
#
# The station's half of the AP harness's bench cell, and it carries the same
# caveat: a flood ping is ROUND-TRIP bound, so reply_pps is a LATENCY figure
# and not link throughput. Nothing in this tree measures throughput. What is
# deterministic here is the per-frame CPU cost, which barely moves with RF
# conditions - see docs/station-mode-plan.md's note on what belongs in a gate.
cell_bench() {
  say "== software CCMP cost (station side, ${BENCH_PAYLOAD}B payload, ${BENCH_SECS}s) =="
  case "$BENCH_PAYLOAD" in
    ''|*[!0-9]*) bad "bench: BENCH_PAYLOAD must be an integer"; return ;;
  esac
  [ "$BENCH_PAYLOAD" -ge 0 ] && [ "$BENCH_PAYLOAD" -le 1400 ] || {
    bad "bench: BENCH_PAYLOAD must be 0..1400 (avoid IP fragmentation)"; return; }
  build_sta || { bad "bench: build"; return; }
  ns_up || { bad "bench: could not move $AP_PHY into $NS"; return; }
  # No rekey during a measurement - it would land inside one of the windows
  # and be indistinguishable from noise.
  REKEY_S=86400 PTK_REKEY_S=0 ap_up wpa2 ||
    { bad "bench: hostapd did not come up"; return; }

  # Two measurement windows - one idle, one busy - on top of SECS.
  local run_secs=$(( SECS + BENCH_SECS * 2 ))
  sta_up "$run_secs" DEVOURER_STA_PSK="$PSK" DEVOURER_CCMP_PROFILE=1 ||
    { bad "bench: sta_client did not start"; ap_down; return; }
  tap_up || { kill "$STA_PID" 2>/dev/null; ap_down; return; }
  wait_for "EAPOL-4WAY-HS-COMPLETED" "$OUT/hostapd.log" 30 || {
    bad "bench: the four-way did not complete"; kill "$STA_PID" 2>/dev/null; ap_down; return; }

  # `$STA_PID` is coreutils timeout; the CPU belongs to its direct child.
  local cpu_pid; cpu_pid=$(pgrep -P "$STA_PID" -x sta_client | head -1)
  [ -n "$cpu_pid" ] || { bad "bench: cannot find the sta_client child"; kill "$STA_PID" 2>/dev/null; ap_down; return; }

  ping -c 1 -W 3 -I "$TAP" "$APIP" >/dev/null 2>&1
  proc_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null; }
  sys_busy() { awk '/^cpu / { idle=$5+$6; for(i=2;i<=NF;i++) total+=$i; print total-idle }' /proc/stat; }
  local hz; hz=$(getconf CLK_TCK)
  local ib0 ib1 wb0 wb1 ip0 ip1 wp0 wp1
  # The paired IDLE window is what makes the busy one mean anything: it
  # removes the RX loop, the beacons and the host's background from the
  # figure, none of which the measurement is about.
  ib0=$(sys_busy); ip0=$(proc_ticks "$cpu_pid"); sleep "$BENCH_SECS"
  ib1=$(sys_busy); ip1=$(proc_ticks "$cpu_pid")
  wb0=$(sys_busy); wp0=$(proc_ticks "$cpu_pid")
  ping -f -q -s "$BENCH_PAYLOAD" -w "$BENCH_SECS" -I "$TAP" "$APIP" \
      >"$OUT/bench.ping" 2>&1 || true
  wb1=$(sys_busy); wp1=$(proc_ticks "$cpu_pid")

  local tx rx loss sta_core sys_cores
  tx=$(sed -n 's/^\([0-9][0-9]*\) packets transmitted.*/\1/p' "$OUT/bench.ping" | tail -1)
  rx=$(sed -n 's/.* \([0-9][0-9]*\) received.*/\1/p' "$OUT/bench.ping" | tail -1)
  loss=$(sed -n 's/.* \([0-9.][0-9.]*%\) packet loss.*/\1/p' "$OUT/bench.ping" | tail -1)
  tx=${tx:-0}; rx=${rx:-0}; loss=${loss:-unknown}
  sta_core=$(awk -v w="$((wp1-wp0))" -v i="$((ip1-ip0))" -v h="$hz" -v s="$BENCH_SECS" \
      'BEGIN { v=(w-i)/h/s*100; if(v<0)v=0; printf "%.2f",v }')
  sys_cores=$(awk -v w="$((wb1-wb0))" -v i="$((ib1-ib0))" -v h="$hz" -v s="$BENCH_SECS" \
      'BEGIN { v=(w-i)/h/s; if(v<0)v=0; printf "%.3f",v }')
  if [ "${rx:-0}" -gt 0 ] && [ "$rx" -ge $(( tx / 2 )) ]; then
    ok "bench: $rx/$tx replies, loss=$loss, station=${sta_core}% core, system_increment=${sys_cores} cores"
  else
    bad "bench: only $rx/$tx replies (loss=$loss) - too few to time anything; station=${sta_core}% core"
  fi

  kill "$STA_PID" 2>/dev/null
  sta_wait
  local profile; profile=$(grep '"ev":"ccmp.profile"' "$OUT/sta.log" | tail -1)
  ap_down
  [ -n "$profile" ] || { bad "bench: missing ccmp.profile"; return; }
  printf '%s\n' "$profile"
  # A PROFILE OF ZERO FRAMES IS NOT A MEASUREMENT, and this cell used to print
  # one and pass. The first bench run reported ccmp_tx_ns_per_frame=0 over
  # 3365 encrypted round trips, because the profiling CryptoOps existed and
  # nothing instantiated it - the reply count was the only thing graded, and
  # replies say nothing about whether anything was timed.
  # FRAMES AND NANOSECONDS BOTH: frames>0 with ns=0 is a profile that
  # counted but never timed - the hole tests/sta_d2d_onair.sh's bench closed.
  local pf_tx pf_rx pf_txns pf_rxns
  pf_tx=$(printf '%s' "$profile" | sed -n 's/.*"tx_frames":\([0-9][0-9]*\).*/\1/p')
  pf_rx=$(printf '%s' "$profile" | sed -n 's/.*"rx_frames":\([0-9][0-9]*\).*/\1/p')
  pf_txns=$(printf '%s' "$profile" | sed -n 's/.*"tx_ns":\([0-9][0-9]*\).*/\1/p')
  pf_rxns=$(printf '%s' "$profile" | sed -n 's/.*"rx_ns":\([0-9][0-9]*\).*/\1/p')
  if [ "${pf_tx:-0}" -gt 0 ] 2>/dev/null && [ "${pf_rx:-0}" -gt 0 ] 2>/dev/null &&
     [ "${pf_txns:-0}" -gt 0 ] 2>/dev/null && [ "${pf_rxns:-0}" -gt 0 ] 2>/dev/null; then
    ok "bench: the cipher was actually timed (tx_frames=$pf_tx rx_frames=$pf_rx tx_ns=$pf_txns rx_ns=$pf_rxns)"
  else
    bad "bench: ccmp.profile counted tx_frames=$pf_tx rx_frames=$pf_rx tx_ns=$pf_txns rx_ns=$pf_rxns - nothing was timed, so the ns/frame figures below are meaningless"
  fi
  python3 - "$profile" "$BENCH_PAYLOAD" "$BENCH_SECS" "$tx" "$rx" "$sta_core" "$sys_cores" <<'PYEOF'
import json, sys
p = json.loads(sys.argv[1])
def ns_per(which):
    n = p[f"{which}_frames"]
    return p[f"{which}_ns"] / n if n else 0
row = {"ev":"mt7612u.sta_ccmp_bench", "path":p["path"],
       "ping_payload":int(sys.argv[2]), "seconds":int(sys.argv[3]),
       "requests":int(sys.argv[4]), "replies":int(sys.argv[5]),
       "reply_pps":int(sys.argv[5])/int(sys.argv[3]),
       "sta_incremental_core_pct":float(sys.argv[6]),
       "system_incremental_cores":float(sys.argv[7]),
       "ccmp_tx_ns_per_frame":ns_per("tx"),
       "ccmp_rx_ns_per_frame":ns_per("rx")}
print(json.dumps(row, separators=(",",":")))
PYEOF
}

# Expected check counts, so the advertised score is machine-enforced instead
# of counted by eye. A future edit that dropped an ok()/bad() pair would
# otherwise run one check fewer, exit 0, and still be reported as "N/N".
case "$CELLS" in
  open)      cell_open;      want=5 ;;
  wpa2)      cell_wpa2;      want=6 ;;
  reconnect) cell_reconnect; want=5 ;;
  bench)     cell_bench;     want=2 ;;
  all)       cell_open; cleanup; cell_wpa2; cleanup; cell_reconnect; want=16 ;;
  *)         echo "usage: $0 [open|wpa2|reconnect|bench|all]"; exit 2 ;;
esac

say ""
say "=== $pass passed, $fail failed   (logs: $OUT) ==="
say "=== MT7612U station on ch$CH/$FREQ against hostapd on $AP_IF ($AP_PHY) ==="
if [ "$fail" -eq 0 ] && [ "$pass" -ne "$want" ]; then
  say "=== HARNESS ERROR: $pass checks passed, none failed, but $want were expected ==="
  say "=== a check went missing - do NOT read this as $want/$want ==="
  exit 2
fi
[ -n "$NM_AP" ] && nmcli device set "$AP_IF" managed yes >/dev/null 2>&1
exit $(( fail > 0 ))
