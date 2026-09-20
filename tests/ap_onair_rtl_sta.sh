#!/bin/sh
# AP on-air cells driven against a REALTEK station.
#
# tests/mt7612u_ap_onair.sh expects an MT7612U station at a sysfs path and
# drives it with iw/wpa_supplicant. This is the same acceptance shape with an
# RTL8812AU station on the in-tree-ish rtw_8812au driver, which is a stronger
# witness for exactly the reason docs/mt7612u-ap-mode.md gives for its own
# weakness: its station was the same silicon as its AP, so it was never an
# independent-generation witness. This one is.
#
#   sudo AP_SYSFS=7-1 STA_SYSFS=1-1 CH=6 tests/ap_onair_rtl_sta.sh open
#   sudo AP_SYSFS=7-1 STA_SYSFS=1-1 CH=6 tests/ap_onair_rtl_sta.sh wpa2
#
# Cells: open | wpa2 | both.
#
# THREE THINGS THIS ENCODES, each of which cost a run to learn:
#
#  1. The station adapter must be RE-ENUMERATED, not driver-bound. devourer's
#     libusb claim leaves the interface on usbfs, and writing to a driver's
#     `bind` does not re-probe it - the bind reports success and no netdev
#     appears. An authorized toggle is what brings it back.
#  2. DEVOURER_BCN_TU=25. docs/ap-mode.md says a dense beacon is needed so a
#     fast channel-hopping supplicant scan catches the AP; the MediaTek harness
#     hardcodes 100 TU, which its own station tolerated and this one does not.
#  3. Ping as soon as the supplicant reports CONNECTED, and truncate its log
#     first. This station drops on inactivity (reason 4) within seconds of a
#     completed 4-way, so a fixed sleep straddles the timer and the run reads
#     as a data-plane failure when nothing was ever sent. A stale
#     CTRL-EVENT-CONNECTED in an appended log matches instantly and produces
#     the same wrong answer from the other direction.
#
# 5 GHz caveat: under many regulatory domains the 5 GHz channels are no-IR, and
# `iw connect` fails with -22 even though the beacon is visible in a scan. The
# open cell is known to complete on 2.4 GHz.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
OUT="${OUT:-/tmp/ap-onair-rtl}"
CELLS="${1:-both}"

AP_SYSFS="${AP_SYSFS:-7-1}"
STA_SYSFS="${STA_SYSFS:-1-1}"
CH="${CH:-6}"
PSK="${PSK:-devourer123}"
SECS="${SECS:-58}"
FW_DIR="${FW_DIR:-/lib/firmware/mediatek}"
APIP=192.168.99.1
STAIP=192.168.99.2

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
mkdir -p "$OUT"
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

AP_BUS=${AP_SYSFS%%-*}
AP_PORT=${AP_SYSFS#*-}

build() { # $1 = stem, $2 = out, $3.. = extra libs
  src="$1"; out="$2"; shift 2
  g++ -std=c++20 -O2 -I"$ROOT/src" -I"$ROOT/examples/common" -I"$ROOT/tests" \
      "$ROOT/tests/$src.cpp" "$ROOT/examples/common/env_config.cpp" \
      "$ROOT/examples/common/usb_select.cpp" "$BUILD/libdevourer.a" \
      $(pkg-config --cflags --libs libusb-1.0) "$@" -lpthread -o "/tmp/$out"
}

# See note 1: re-enumerate, do not bind.
sta_up() {
  echo 0 > "/sys/bus/usb/devices/$STA_SYSFS/authorized" 2>/dev/null
  sleep 3
  echo 1 > "/sys/bus/usb/devices/$STA_SYSFS/authorized" 2>/dev/null
  sleep 10
  STA_IF=$(ls "/sys/bus/usb/devices/$STA_SYSFS:1.0/net/" 2>/dev/null | head -1)
  [ -n "${STA_IF:-}" ] || return 1
  ip link set "$STA_IF" up 2>/dev/null
  return 0
}

start_ap() { # $1 = binary, $2.. = extra env
  ( DEVOURER_VID=0x0e8d DEVOURER_PID=0x7612 \
    DEVOURER_USB_BUS="$AP_BUS" DEVOURER_USB_PORT="$AP_PORT" \
    DEVOURER_MT7612U_FW_DIR="$FW_DIR" \
    DEVOURER_CHANNEL="$CH" DEVOURER_BCN_TU=25 \
    DEVOURER_TX_WITH_RX=thread "$@" \
    "/tmp/$1" "$SECS" > "$OUT/$1.jsonl" 2> "$OUT/$1.log" ) &
  AP=$!
  sleep 25
  grep -qE 'up on ch|ap_wpa2 up' "$OUT/$1.log"
}

cell_open() {
  echo "== open network (RTL station) =="
  build ap_responder apr_rtl   || { bad "open: build"; return; }
  sta_up                        || { bad "open: no station interface"; return; }
  start_ap apr_rtl              || { bad "open: AP did not come up"; kill $AP 2>/dev/null; return; }
  ok "open: AP up on ch$CH"

  iw dev "$STA_IF" scan flush 2>/dev/null | grep -q 'SSID: devourerAP' \
    && ok "open: beacon scannable by an independent-silicon station" \
    || bad "open: beacon not scannable"

  iw dev "$STA_IF" connect -w devourerAP >/dev/null 2>&1
  sleep 4
  grep -q 'ASSOC req' "$OUT/apr_rtl.log" \
    && ok "open: station authenticated and associated" \
    || bad "open: no association"
  iw dev "$STA_IF" disconnect 2>/dev/null
  kill $AP 2>/dev/null; wait $AP 2>/dev/null
}

cell_wpa2() {
  echo "== WPA2-PSK + encrypted data plane (RTL station) =="
  build ap_wpa2 apw_rtl -lcrypto || { bad "wpa2: build"; return; }
  sta_up                          || { bad "wpa2: no station interface"; return; }
  start_ap apw_rtl DEVOURER_WPA2_PSK="$PSK" \
    || { bad "wpa2: AP did not come up"; kill $AP 2>/dev/null; return; }
  ok "wpa2: AP up on ch$CH"

  cat > "$OUT/wpa.conf" <<EOF
ctrl_interface=/run/wpa_supplicant_dvr
network={
  ssid="devourerAP"
  psk="$PSK"
  key_mgmt=WPA-PSK
  proto=RSN
  pairwise=CCMP
  group=CCMP
  scan_ssid=1
}
EOF
  : > "$OUT/wpa.log"          # see note 3: a stale CONNECTED matches instantly
  ip addr flush dev "$STA_IF" 2>/dev/null
  ip addr add "$STAIP/24" dev "$STA_IF" 2>/dev/null
  wpa_supplicant -i "$STA_IF" -c "$OUT/wpa.conf" -f "$OUT/wpa.log" -B >/dev/null 2>&1

  i=0
  while [ $i -lt 40 ]; do
    grep -q 'CTRL-EVENT-CONNECTED' "$OUT/wpa.log" 2>/dev/null && break
    sleep 1; i=$((i+1))
  done
  grep -q 'Key negotiation completed' "$OUT/wpa.log" \
    && ok "wpa2: 4-way handshake completed (supplicant)" \
    || bad "wpa2: no key negotiation"
  grep -q '4-WAY HANDSHAKE COMPLETE' "$OUT/apw_rtl.log" \
    && ok "wpa2: 4-way handshake completed (AP side, MIC verified)" \
    || bad "wpa2: AP did not complete the 4-way"

  # Immediately - this station idles out within seconds (note 3).
  if ping -c 5 -W 2 -I "$STA_IF" "$APIP" 2>&1 | grep -q ' 0% packet loss'; then
    ok "wpa2: encrypted ping 0% loss"
  else
    bad "wpa2: encrypted ping lost packets (see the ledger below)"
  fi

  wpa_cli -p /run/wpa_supplicant_dvr -i "$STA_IF" terminate >/dev/null 2>&1
  wait $AP 2>/dev/null
  # The ledger is what separates "never received a frame" from "received and
  # failed to decrypt". Without it both read as 100% packet loss.
  ledger=$(grep 'data plane:' "$OUT/apw_rtl.log")
  echo "  $ledger"
  case "$ledger" in
    *"MIC failures=0"*) ok "wpa2: zero MIC failures" ;;
    *)                  bad "wpa2: MIC failures present" ;;
  esac
  ip addr flush dev "$STA_IF" 2>/dev/null
}

case "$CELLS" in
  open) cell_open ;;
  wpa2) cell_wpa2 ;;
  both) cell_open; cell_wpa2 ;;
  *) echo "usage: $0 [open|wpa2|both]"; exit 2 ;;
esac

echo
echo "=== $pass passed, $fail failed   (logs: $OUT) ==="
[ "$fail" = 0 ]
