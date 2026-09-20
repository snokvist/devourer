#!/bin/sh
# mt7612u_sta_uplink.sh - is an MT7612U station's OWN traffic acknowledged?
#
# The other half of AdapterCaps::station_mode_ok's bar.
# tests/mt7612u_sta_autoack.sh measures frames sent TO the DUT; this measures
# frames sent BY it. A station whose uplink is never acknowledged retransmits
# everything to the retry limit and gives up, which looks like a link problem
# and is not.
#
# Instrument: the DUT's own MT_TX_STAT_FIFO, via `bringup txs`, which reports
# the MAC's per-MPDU retry count. The peer is a Realtek adapter running rxdemo
# with DEVOURER_ACK_RESPONDER armed on a chosen address - the same responder
# tests/ack_txreport_matrix.sh uses.
#
# Two arms, and the second is what makes the first mean anything:
#
#   A  peer armed on the address we transmit to   -> expect retries ~0
#   B  peer armed on a DIFFERENT address          -> expect retries pinned
#
# B is the control. It holds the peer present, on channel, and transmitting
# nothing different - only the address it answers for changes. Without it,
# "few retries" might be what this channel always gives.
#
#   sudo tests/mt7612u_sta_uplink.sh
#
# Env: PEER_VID, PEER_PID, PEER_SYSFS, DUT_SYSFS, CH, FRAMES, OUT.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
PEER_VID="${PEER_VID:-0x0bda}"
PEER_PID="${PEER_PID:-0xc812}"
PEER_SYSFS="${PEER_SYSFS:-5-1}"
DUT_SYSFS="${DUT_SYSFS:-7-1}"
CH="${CH:-6}"
FRAMES="${FRAMES:-60}"
OUT="${OUT:-/tmp/mt7612u-sta-uplink}"
FW_DIR="${FW_DIR:-/lib/firmware/mediatek}"
# The address the DUT transmits to. The peer answers for this in arm A and for
# OTHER in arm B.
TARGET="${TARGET:-02:aa:bb:cc:dd:11}"
OTHER="${OTHER:-02:aa:bb:cc:dd:12}"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
mkdir -p "$OUT"
[ -e "$ROOT/firmware" ] || ln -sfn "$FW_DIR" "$ROOT/firmware" 2>/dev/null

pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

RESP=""
cleanup() { [ -n "$RESP" ] && kill "$RESP" 2>/dev/null; rm -f "$ROOT/firmware"; }
trap cleanup EXIT INT TERM

echo "$DUT_SYSFS:1.0" > /sys/bus/usb/drivers/mt76x2u/unbind 2>/dev/null
sleep 2
echo "DUT  MT7612U at $DUT_SYSFS transmitting to $TARGET"
echo "peer $PEER_VID:$PEER_PID at $PEER_SYSFS, ch$CH"
echo

# $1 = tag, $2 = the address the peer answers for
arm() {
  tag="$1"; resp="$2"
  env DEVOURER_VID="$PEER_VID" DEVOURER_PID="$PEER_PID" \
      DEVOURER_USB_BUS="${PEER_SYSFS%%-*}" DEVOURER_USB_PORT="${PEER_SYSFS#*-}" \
      DEVOURER_CHANNEL="$CH" DEVOURER_ACK_RESPONDER="$resp" \
      DEVOURER_LOG_LEVEL=info \
      "$BUILD/rxdemo" >"$OUT/resp_$tag.jsonl" 2>"$OUT/resp_$tag.err" &
  RESP=$!
  sleep 10
  if ! kill -0 "$RESP" 2>/dev/null; then
    printf '%s ABORTED the peer exited: %s' "$tag" "$(tail -1 "$OUT/resp_$tag.err")"
    RESP=""; return 1
  fi
  # The arm must be VERIFIED, not assumed: an unarmed responder and a
  # responder armed on the wrong address look identical from here, and that is
  # exactly what arm B is supposed to be.
  if ! grep -qi "ack responder" "$OUT/resp_$tag.err"; then
    printf '%s ABORTED the peer never reported arming a responder' "$tag"
    kill "$RESP" 2>/dev/null; RESP=""; return 1
  fi

  "$BUILD/mt7612uprobe" txs "$CH" "$FRAMES" "$TARGET" \
      >"$OUT/dut_$tag.txt" 2>&1
  kill "$RESP" 2>/dev/null; wait "$RESP" 2>/dev/null; RESP=""

  # `bringup txs` prints the arm table TWICE - once with the MAC receiver OFF
  # and once with it ON. That A/B is Phase 0's result built into the gate: with
  # the receiver off the MAC cannot hear an ACK, so every unicast arm runs its
  # retry ladder to exhaustion regardless of what the peer does.
  #
  # A station runs with its receiver on, so the "MAC receiver ON" table is the
  # only one that answers this question. Reading the first arm-d row instead
  # of the right one reported UNSETTLED for both arms and threw away a clean
  # measurement.
  #
  # The row is arm `d`, "ucast peer ownSA Normal": transmitted from our OWN
  # address to the peer with normal ack policy, which is what a station's
  # uplink is. Columns: fps, entries/sent, success, mean retries, max retries,
  # plus an UNSETTLED marker when too few frames were sent for the 16-slot
  # status ring to attribute cleanly.
  python3 - "$OUT/dut_$tag.txt" "$tag" <<'PYEOF'
import re, sys
path, tag = sys.argv[1], sys.argv[2]

section = None
row = None
for line in open(path, errors='replace'):
    if 'MAC receiver' in line:
        section = 'ON' if 'ON' in line else 'OFF'
        continue
    if section == 'ON' and re.match(r'\s*d\s+ucast peer ownSA Normal', line):
        row = line
        break

if row is None:
    print(f"{tag} NOPARSE no receiver-ON arm-d row in {path}")
    sys.exit()
m = re.search(r'(\d+)\s*/\s*(\d+)\s+(\d+)\s+([\d.]+)\s+(\d+)', row)
if not m:
    print(f"{tag} NOPARSE arm-d row unreadable: {row.strip()}")
    sys.exit()
entries, sent, success, mean_rtry, max_rtry = m.groups()
if 'UNSETTLED' in row and int(success) > 0:
    # UNSETTLED means fewer status entries landed than frames were sent, so
    # the gate cannot guarantee each entry belongs to the arm it is printed
    # under. That matters when an arm CLAIMS SUCCESS - refuse it.
    #
    # It does not matter for an arm that succeeded at nothing, and refusing
    # those would make this measurement impossible: the failing control is
    # UNSETTLED BY CONSTRUCTION. When no peer acknowledges, every frame runs
    # the full 16-retry ladder, the MAC is roughly two orders of magnitude
    # slower per frame, and the 16-slot status ring can never keep up with
    # submission. A control that always reads UNSETTLED is not a control.
    #
    # The direction of the risk also points the safe way. Misattributed
    # entries would come from the NEIGHBOURING arms, which in this table run
    # at 200/200 and zero retries - so contamination can only make a failing
    # arm look BETTER. An arm reading 0 success at max retries is therefore a
    # floor, and a floor is all a control needs to be.
    print(f"{tag} UNSETTLED entries={entries}/{sent} success={success} "
          f"retries={mean_rtry} - success claimed on unreliable attribution")
    sys.exit()
sent_i = int(sent) or 1
note = "  [entries<sent: a floor, see the note in this script]" \
       if 'UNSETTLED' in row else ""
print(f"{tag} acked={success}/{sent} ok_pct={100.0*int(success)/sent_i:.1f} "
      f"retries={mean_rtry} max={max_rtry}{note}")
PYEOF
}

echo "== A: peer answers for $TARGET (the address we transmit to) =="
a=$(arm A "$TARGET"); echo "  $a"
echo "== B: peer answers for $OTHER instead (control) =="
b=$(arm B "$OTHER"); echo "  $b"
echo

for pair in "A:$a" "B:$b"; do
  t=${pair%%:*}; v=${pair#*:}
  case "$v" in
    *ABORTED*) echo "ARM $t ABORTED: ${v#* ABORTED }"; echo "GATE UPLINK: INCONCLUSIVE"; exit 2 ;;
    *NOPARSE*) echo "ARM $t produced nothing parseable - not a measurement."
               echo "GATE UPLINK: INCONCLUSIVE"; exit 2 ;;
    *UNSETTLED*) echo "ARM $t: $v"
               echo "The gate flagged its own attribution as unreliable, so"
               echo "these numbers are not a measurement. Raise FRAMES."
               echo "GATE UPLINK: INCONCLUSIVE"; exit 2 ;;
  esac
done

a_ok=$(printf '%s' "$a" | sed -n 's/.*ok_pct=\([0-9.]*\).*/\1/p')
b_ok=$(printf '%s' "$b" | sed -n 's/.*ok_pct=\([0-9.]*\).*/\1/p')
awk -v a="${a_ok:-0}" -v b="${b_ok:-0}" 'BEGIN{
  printf "A (peer answers for us) acked=%.1f%%\nB (peer answers elsewhere) acked=%.1f%%\n", a, b
  exit !(a > b + 40)
}' && ok "the station's own uplink is acknowledged, and the control shows the gate can fail" \
   || bad "A is not clearly above the control - the uplink is not shown to be acknowledged"

echo
echo "=== $pass passed, $fail failed  (logs: $OUT) ==="
exit $(( fail > 0 ))
