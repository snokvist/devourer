#!/usr/bin/env bash
# mt7612u_sta_identity.sh - Phase 2's two measurements, end to end.
#
# Answers the two risks docs/station-mode-scope.md raised against the backend
# half of SetStationIdentity and left unmeasured:
#
#   R5  what does programming the BSSID do for a MANAGED STATION?
#   R6  does this MAC auto-ACK unicast to its own address with nothing armed,
#       and what happens to that if MT_MAC_ADDR is moved?
#
# Results and how to read them: docs/mt7612u-station-identity.md.
#
# Rig: a second adapter with AP-mode support runs hostapd and is the AP; the
# MT7612U is the device under test and is driven by build/mt7612uprobe. The
# AP's BSSID is deliberately LOCALLY ADMINISTERED - mt76 derives the APC slot
# as idx = 1 + (((mbss_base[0] ^ addr[0]) >> 2) & 7) for a locally
# administered address and 0 otherwise, so a factory BSSID would collapse the
# "slot 0" and "derived slot" arms into the same test.
#
#   sudo tests/mt7612u_sta_identity.sh
#   sudo AP_SYSFS=1-1 DUT_SYSFS=7-1 CH=6 tests/mt7612u_sta_identity.sh
#
# Env: AP_SYSFS, DUT_SYSFS, CH, BSSID, SECS, OUT, FW_DIR.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
# The bring-up tool resolves its firmware directory RELATIVE TO THE WORKING
# DIRECTORY ("firmware/mt7662_rom_patch.bin"), and the symlink below is created
# at $ROOT. Running this script from anywhere else therefore fails the DUT's
# firmware load, which surfaces as "could not read the DUT's MAC" - a message
# that names neither the cause nor the cure. Pin the directory instead.
cd "$ROOT" || exit 1
AP_SYSFS="${AP_SYSFS:-1-1}"
DUT_SYSFS="${DUT_SYSFS:-7-1}"
CH="${CH:-6}"
BSSID="${BSSID:-02:42:75:05:d6:aa}"
SECS="${SECS:-20}"
OUT="${OUT:-/tmp/mt7612u-sta-identity}"
FW_DIR="${FW_DIR:-/lib/firmware/mediatek}"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
command -v hostapd >/dev/null || { echo "hostapd is required"; exit 2; }
mkdir -p "$OUT"

# mt7612uprobe takes no firmware-directory argument and looks for ./firmware,
# so give it one rather than requiring the caller to cd somewhere specific.
# FW_LINK_OURS: only a link THIS run created is removed afterwards - a
# pre-existing $ROOT/firmware is the operator's.
FW_LINK_OURS=no
if [ ! -e "$ROOT/firmware" ] && ln -sfn "$FW_DIR" "$ROOT/firmware" 2>/dev/null; then
  FW_LINK_OURS=yes
fi

AP_IF=""
# Set only once hostapd is up on a verified AP-capable interface: before
# that, the trap has no business re-enumerating anything (a wrong or default
# AP_SYSFS naming a hub would power-cycle every device under it).
AP_REENUM=no
CLEANED=no
cleanup() {
  [ "$CLEANED" = yes ] && return 0
  CLEANED=yes
  [ "$FW_LINK_OURS" = yes ] && rm -f "$ROOT/firmware"
  [ "$AP_REENUM" = yes ] || return 0
  pkill -f "hostapd.*$OUT/hostapd.conf" 2>/dev/null
  sleep 1
  iw dev staid_mon del 2>/dev/null
  # RE-ENUMERATE the AP adapter, do not just bounce the link.
  #
  # hostapd's `bssid=` leaves the interface carrying that address after it
  # exits, and the adapter does not recover from `ip link down/up` - it comes
  # back still holding the BSSID, DOWN, and scanning nothing. Left that way it
  # silently breaks the next harness that expects this adapter to be a
  # station, which is how it was found: three spurious failures in
  # tests/mt7612u_ap_onair.sh on the run straight after this one.
  if [ -n "${AP_SYSFS:-}" ] && [ -e "/sys/bus/usb/devices/$AP_SYSFS/authorized" ]; then
    echo 0 > "/sys/bus/usb/devices/$AP_SYSFS/authorized" 2>/dev/null
    sleep 3
    echo 1 > "/sys/bus/usb/devices/$AP_SYSFS/authorized" 2>/dev/null
    sleep 8
  fi
  AP_IF=$(ls "/sys/bus/usb/devices/$AP_SYSFS:1.0/net/" 2>/dev/null | head -1)
  [ -n "$AP_IF" ] && {
    rfkill unblock wlan 2>/dev/null
    ip link set "$AP_IF" up 2>/dev/null
    nmcli device set "$AP_IF" managed yes >/dev/null 2>&1
  }
}
trap cleanup EXIT
# AND IT MUST STOP: with INT/TERM on the EXIT trap the shell runs cleanup
# and then CARRIES ON into the next arm (tests/sta_d2d_onair.sh found and
# fixed this). cleanup is idempotent, so the EXIT pass after it is harmless.
trap 'cleanup; exit 130' INT TERM

# --- the AP ----------------------------------------------------------------
AP_IF=$(ls "/sys/bus/usb/devices/$AP_SYSFS:1.0/net/" 2>/dev/null | head -1)
if [ -z "$AP_IF" ]; then
  echo "$AP_SYSFS:1.0" > /sys/bus/usb/drivers_probe 2>/dev/null
  sleep 3
  AP_IF=$(ls "/sys/bus/usb/devices/$AP_SYSFS:1.0/net/" 2>/dev/null | head -1)
fi
[ -n "$AP_IF" ] || { echo "no AP interface at $AP_SYSFS"; exit 2; }
# The same guards tests/mt7612u_sta_onair.sh applies before it re-enumerates:
# a plausible device, not a hub, not the DUT's own path.
ap_cls=$(cat "/sys/bus/usb/devices/$AP_SYSFS/bDeviceClass" 2>/dev/null)
ap_vid=$(cat "/sys/bus/usb/devices/$AP_SYSFS/idVendor" 2>/dev/null)
if [ -z "$ap_vid" ] || [ "$AP_SYSFS" = "$DUT_SYSFS" ] || [ "$ap_cls" = "09" ]; then
  echo "refusing AP_SYSFS=$AP_SYSFS (vid='$ap_vid' class='$ap_cls') - not a"
  echo "plausible AP device, and cleanup would re-enumerate it. Check AP_SYSFS."
  exit 2
fi
PHY=$(basename "$(readlink -f "/sys/class/net/$AP_IF/phy80211")")
iw phy "$PHY" info 2>/dev/null | grep -q '\* AP$' || {
  echo "$AP_IF ($PHY) does not support AP mode - this harness needs a"
  echo "second adapter that does."; exit 2; }

echo "AP  $AP_IF ($PHY) bssid $BSSID ch$CH"
echo "DUT $DUT_SYSFS (MT7612U)"

# hostapd and NetworkManager fight over the interface; and a previous run can
# leave the vif in AP type, which makes hostapd fail with "Match already
# configured" rather than anything that names the real problem.
nmcli device set "$AP_IF" managed no >/dev/null 2>&1
sleep 1
ip link set "$AP_IF" down 2>/dev/null
iw dev "$AP_IF" set type managed 2>/dev/null
ip link set "$AP_IF" up 2>/dev/null

cat > "$OUT/hostapd.conf" <<EOF
interface=$AP_IF
driver=nl80211
ssid=staidentity
bssid=$BSSID
hw_mode=g
channel=$CH
auth_algs=1
wmm_enabled=0
EOF
hostapd -B -f "$OUT/hostapd.log" "$OUT/hostapd.conf" >/dev/null 2>&1
sleep 5
grep -q "AP-ENABLED" "$OUT/hostapd.log" 2>/dev/null || {
  AP_REENUM=yes   # hostapd may have run and left its bssid behind
  echo "hostapd did not come up:"; tail -12 "$OUT/hostapd.log"; exit 1; }
AP_REENUM=yes

# --- free the DUT ----------------------------------------------------------
echo "$DUT_SYSFS:1.0" > /sys/bus/usb/drivers/mt76x2u/unbind 2>/dev/null
sleep 2

# The contract gate needs no AP, prints the DUT's own address, and is the
# cheapest thing that fails loudly if the DUT is not usable - so it runs
# first and doubles as this harness's source for the MAC. (`regs` does not
# work for this: it never calls mt_eeprom_init, so it prints no MAC.)
echo
echo "########## the SetStationIdentity contract (no AP needed) ##########"
# PIPESTATUS[0], captured straight after each pipeline: `$?` is tee's
# status, and under the old #!/bin/sh (dash) PIPESTATUS did not exist at all,
# so a failing probe scored as a pass.
"$BUILD/mt7612uprobe" staid 2>&1 | tee "$OUT/staid.txt"
staid=${PIPESTATUS[0]}
DUT_MAC=$(sed -n 's/^own \([0-9a-f:]\{17\}\).*/\1/p' "$OUT/staid.txt" | head -1)
[ -n "$DUT_MAC" ] || { echo "could not read the DUT's MAC from the staid gate"; exit 1; }
echo "DUT MAC $DUT_MAC"

# --- R6 first: no monitor vif needed, and it is the sharper result ---------
echo
echo "########## R6: does the DUT auto-ACK with nothing armed? ##########"
"$BUILD/mt7612uprobe" staack "$CH" "$SECS" "$BSSID" 2>&1 | tee "$OUT/r6.txt"
r6=${PIPESTATUS[0]}

# --- R5: needs unicast aimed at the DUT for the whole run ------------------
echo
echo "########## R5: what does programming the BSSID change? ##########"
iw dev staid_mon del 2>/dev/null
if iw phy "$PHY" interface add staid_mon type monitor 2>/dev/null &&
   ip link set staid_mon up 2>/dev/null; then
  # Without this every arm reads to_us=0: hostapd sends an unassociated
  # station no unicast, so the gate would measure broadcast reception only
  # and could not answer the question it exists for.
  python3 "$ROOT/tests/sta_unicast_inject.py" staid_mon "$DUT_MAC" "$BSSID" \
      $(( SECS * 6 + 40 )) 300 > "$OUT/inject.log" 2>&1 &
  sleep 2
  echo "(unicast injector running on staid_mon)"
else
  echo "WARNING: no monitor vif - R5 will only measure broadcast reception,"
  echo "         which is not the question. Treat its to_us column as void."
fi
"$BUILD/mt7612uprobe" sta "$CH" "$SECS" "$BSSID" 2>&1 | tee "$OUT/r5.txt"
r5=${PIPESTATUS[0]}

echo
echo "=== logs: $OUT ==="
[ "${staid:-0}" = 0 ] || echo "the contract gate FAILED - see $OUT/staid.txt"
[ "$r6" = 0 ] || echo "R6 did not pass - see $OUT/r6.txt"
[ "$r5" = 0 ] || echo "R5 did not pass - see $OUT/r5.txt"
exit $(( r5 != 0 || r6 != 0 || ${staid:-0} != 0 ))
