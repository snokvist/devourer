#!/bin/sh
# mt7612u_sta_autoack.sh - does an MT7612U station acknowledge unicast sent to
# its own address, with NOTHING armed?
#
# This is R6, and it is the one Phase 2 question that stayed open.
# docs/station-mode-scope.md asserts the answer is yes, from a register
# reading: the auto-response engine matches address 1 against MT_MAC_ADDR and
# MT_AUTO_RSP_EN is on from init. It has never been measured.
#
# TWO EARLIER METHODS FAILED, recorded so they are not retried:
#
#   1. Capture the DUT's ACKs on a monitor vif on the peer's own phy. Produced
#      ZERO in both the DUT-present and DUT-absent arms - a radio cannot hear
#      an ACK to its own transmission, and mac80211 injects no-ack anyway.
#   2. Make hostapd answer a directed probe request and count RETRIED copies.
#      The single-variable control (clear MT_AUTO_RSP_EN, hold reception
#      constant) did not move, so the method could not fail: this AP simply
#      does not retransmit an unacknowledged probe response.
#
# Both share a flaw: they ask the DUT, or a device that cannot see the answer.
# The transmitter is the only party that knows whether its frame was
# acknowledged, and on Realtek that knowledge is a per-frame CCX report.
#
# So: a Realtek adapter under devourer injects unicast QoS-Data at the DUT and
# reads its own tx.report events. retries~0 means the DUT answered; retries
# pinned at the descriptor limit means it did not. That is the same instrument
# tests/ack_txreport_matrix.sh uses, pointed the other way round - there the
# Realtek part is the responder, here it is the witness.
#
# Jaguar3 (8812CU/8822CU) is the preferred peer because it drains C2H off its
# coex runtime, so the reports arrive without further arrangement. Any
# generation with ack_responder_ok works if it runs DEVOURER_TX_WITH_RX=thread.
#
#   sudo tests/mt7612u_sta_autoack.sh
#   sudo PEER_PID=0xc812 DUT_SYSFS=7-1 CH=6 tests/mt7612u_sta_autoack.sh
#
# Env: PEER_VID, PEER_PID, PEER_SYSFS, DUT_SYSFS, CH, SECS, RETRY_LIMIT, OUT.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
PEER_VID="${PEER_VID:-0x0bda}"
PEER_PID="${PEER_PID:-0xc812}"
PEER_SYSFS="${PEER_SYSFS:-5-1}"
DUT_SYSFS="${DUT_SYSFS:-7-1}"
CH="${CH:-6}"
SECS="${SECS:-10}"
RETRY_LIMIT="${RETRY_LIMIT:-12}"
OUT="${OUT:-/tmp/mt7612u-sta-autoack}"
FW_DIR="${FW_DIR:-/lib/firmware/mediatek}"
# An address nobody holds. The control arm targets this: same transmitter,
# same rate, same channel, only the destination changes.
NOBODY="${NOBODY:-02:00:00:de:ad:07}"
TX_SA="${TX_SA:-02:aa:bb:cc:dd:07}"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
mkdir -p "$OUT"
[ -e "$ROOT/firmware" ] || ln -sfn "$FW_DIR" "$ROOT/firmware" 2>/dev/null

pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

DUT_PID=""
cleanup() { [ -n "$DUT_PID" ] && kill "$DUT_PID" 2>/dev/null; }
trap cleanup EXIT INT TERM

echo "$DUT_SYSFS:1.0" > /sys/bus/usb/drivers/mt76x2u/unbind 2>/dev/null
sleep 2

DUT_MAC=$("$BUILD/mt7612uprobe" staid 2>&1 | sed -n 's/^own \([0-9a-f:]\{17\}\).*/\1/p' | head -1)
[ -n "$DUT_MAC" ] || { echo "could not read the DUT's MAC"; exit 1; }
echo "DUT  MT7612U at $DUT_SYSFS, own $DUT_MAC"
echo "peer $PEER_VID:$PEER_PID at $PEER_SYSFS, ch$CH, retry limit $RETRY_LIMIT"
echo

# $1 = tag, $2 = destination, $3 = 1 if the DUT should be receiving
arm() {
  tag="$1"; ra="$2"; dut_up="$3"
  DUT_PID=""
  if [ "$dut_up" != 0 ]; then
    # 1 = receiver on, managed filter, NOTHING armed - that is the claim.
    # 2 = the same, with MT_AUTO_RSP_EN cleared: one bit different, which is
    #     the single-variable test of WHICH mechanism answers.
    if [ "$dut_up" = 3 ]; then
      "$BUILD/mt7612uprobe" bssen "$CH" $((SECS + 14)) \
          >"$OUT/dut_$tag.log" 2>&1 &
    elif [ "$dut_up" = 2 ]; then
      "$BUILD/mt7612uprobe" norsp "$CH" $((SECS + 14)) \
          >"$OUT/dut_$tag.log" 2>&1 &
    else
      "$BUILD/mt7612uprobe" arx "$CH" $((SECS + 14)) \
          >"$OUT/dut_$tag.log" 2>&1 &
    fi
    DUT_PID=$!
    sleep 8
    # `arm` runs inside a command substitution, so `exit` here would only kill
    # the subshell and the caller would carry on with an EMPTY result - which
    # parses as 0% and reads as a passing control. Emit a marker instead and
    # let the verdicts refuse it.
    kill -0 "$DUT_PID" 2>/dev/null || {
      printf '%s ABORTED the DUT exited before this arm: %s' \
             "$tag" "$(tail -1 "$OUT/dut_$tag.log" 2>/dev/null)"
      return 1; }
  fi

  env DEVOURER_VID="$PEER_VID" DEVOURER_PID="$PEER_PID" \
      DEVOURER_USB_BUS="${PEER_SYSFS%%-*}" DEVOURER_USB_PORT="${PEER_SYSFS#*-}" \
      DEVOURER_CHANNEL="$CH" \
      DEVOURER_TX_QOS_DATA=1 DEVOURER_TX_RA="$ra" DEVOURER_TX_SA="$TX_SA" \
      DEVOURER_TX_RATE=MCS3 DEVOURER_TX_PAYLOAD_BYTES=200 \
      DEVOURER_TX_GAP_US=5000 DEVOURER_TX_REPORT=1 \
      DEVOURER_TX_RETRY_LIMIT="$RETRY_LIMIT" \
      DEVOURER_TX_WITH_RX=thread DEVOURER_LOG_LEVEL=warn \
      timeout -s INT "$SECS" "$BUILD/txdemo" \
      >"$OUT/tx_$tag.jsonl" 2>"$OUT/tx_$tag.err"

  [ -n "$DUT_PID" ] && { kill "$DUT_PID" 2>/dev/null; wait "$DUT_PID" 2>/dev/null; }
  DUT_PID=""

  python3 - "$OUT/tx_$tag.jsonl" "$tag" <<'PYEOF'
import json, sys
path, tag = sys.argv[1], sys.argv[2]
n = okc = 0
retries = 0
for line in open(path, errors='replace'):
    if '"ev":"tx.report"' not in line:
        continue
    try:
        e = json.loads(line)
    except ValueError:
        continue
    n += 1
    if e.get('ok'):
        okc += 1
    retries += int(e.get('retries', 0) or 0)
if not n:
    print(f"{tag} reports=0")
else:
    print(f"{tag} reports={n} ok={okc} ok_pct={100.0*okc/n:.1f} "
          f"retries_mean={retries/n:.2f}")
PYEOF
}

echo "== A: destination is the DUT, DUT receiving, nothing armed =="
a=$(arm A "$DUT_MAC" 1); echo "  $a"
echo "== B: destination is an address NOBODY holds (control) =="
b=$(arm B "$NOBODY" 1); echo "  $b"
echo "== C: destination is the DUT, DUT NOT running (control) =="
c=$(arm C "$DUT_MAC" 0); echo "  $c"
echo "== D: destination is the DUT, DUT receiving, MT_AUTO_RSP_EN CLEARED =="
echo "     (single-variable: which mechanism answers?)"
d=$(arm D "$DUT_MAC" 2); echo "  $d"
echo "== E: destination is the DUT, DUT receiving, WRONG BSSID in an ENABLED APC slot =="
echo "     (R5's last caveat: does an enabled slot gate a station?)"
e=$(arm E "$DUT_MAC" 3); echo "  $e"
echo

rm -f "$ROOT/firmware"

# Verdicts. A must differ from BOTH controls, or the instrument is not
# measuring the DUT.
a_ok=$(printf '%s' "$a" | sed -n 's/.*ok_pct=\([0-9.]*\).*/\1/p')
b_ok=$(printf '%s' "$b" | sed -n 's/.*ok_pct=\([0-9.]*\).*/\1/p')
c_ok=$(printf '%s' "$c" | sed -n 's/.*ok_pct=\([0-9.]*\).*/\1/p')
d_ok=$(printf '%s' "$d" | sed -n 's/.*ok_pct=\([0-9.]*\).*/\1/p')
e_ok=$(printf '%s' "$e" | sed -n 's/.*ok_pct=\([0-9.]*\).*/\1/p')

# EVERY arm must have produced reports. An arm that aborted, or one the peer
# never reported on, yields an empty ok_pct - and an empty value compared
# numerically reads as 0, which is the PASSING value for a control. That is
# how an aborted arm D was scored as "MT_AUTO_RSP_EN is the gate" on the first
# run of this harness.
for pair in "A:$a" "B:$b" "C:$c" "D:$d" "E:$e"; do
  t=${pair%%:*}; v=${pair#*:}
  case "$v" in
    *ABORTED*)   echo "ARM $t ABORTED: ${v#* ABORTED }"
                 echo "GATE AUTOACK: INCONCLUSIVE"; exit 2 ;;
  esac
  n=$(printf '%s' "$v" | sed -n 's/.*reports=\([0-9]*\).*/\1/p')
  if [ -z "${n:-}" ] || [ "${n:-0}" -eq 0 ]; then
    echo "ARM $t produced NO tx.report events, so it is not a measurement and"
    echo "must not be compared. Check the peer runs DEVOURER_TX_WITH_RX=thread"
    echo "and that its generation has a CCX path."
    echo "GATE AUTOACK: INCONCLUSIVE"; exit 2
  fi
done

awk -v a="${a_ok:-0}" -v b="${b_ok:-0}" -v c="${c_ok:-0}" 'BEGIN{
  printf "A (DUT present)        ok=%.1f%%\nB (nobody)             ok=%.1f%%\nC (DUT absent)         ok=%.1f%%\n", a, b, c
  exit !(a > b + 40 && a > c + 40)
}' && ok "the DUT acknowledges unicast to its own address with nothing armed" \
   || bad "A is not clearly above both controls - the DUT is not shown to acknowledge"

# D decides whether SetStationIdentity's MT_AUTO_RSP_EN refusal is justified.
# It is a separate verdict: the claim above stands either way.
awk -v a="${a_ok:-0}" -v d="${d_ok:-0}" 'BEGIN{
  printf "D (AUTO_RSP_EN off)    ok=%.1f%%\n", d
  exit !(d < a - 40)
}' && ok "MT_AUTO_RSP_EN is the gate - SetStationIdentity is right to refuse when it is clear" \
   || bad "MT_AUTO_RSP_EN is NOT the gate here - the seam refuses on a bit that does not control this"

# E closes R5's last caveat: every earlier arm left mt76's per-slot enable
# clear, so "a wrong BSSID changes nothing" could have meant "nothing was
# reading the BSSID". Here the slot is wrong AND enabled.
awk -v a="${a_ok:-0}" -v e="${e_ok:-0}" 'BEGIN{
  printf "E (wrong BSSID, slot ENABLED) ok=%.1f%%\n", e
  exit !(e > a - 20)
}' && ok "a wrong BSSID in an ENABLED APC slot does not gate the station - R5 closed" \
   || bad "an enabled APC slot DOES gate the station - R5's null result was an artefact of the enable bit being clear"

echo
echo "=== $pass passed, $fail failed  (logs: $OUT) ==="
exit $(( fail > 0 ))
