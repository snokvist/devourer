#!/bin/sh
# mt7612u_sta_autoack.sh - does an MT7612U station acknowledge unicast sent to
# its own address, with NOTHING armed?
#
# This is R6. ANSWERED by this script on 2026-09-20: 1279 frames, 100%
# acknowledged at 0.45 mean retries with nothing armed, against three controls
# pinned at the 12-retry limit. docs/mt7612u-station-identity.md has the table
# and its limits - notably that it is a ONE-PEER result.
#
# Before that it was only a register reading (the auto-response engine matches
# address 1 against MT_MAC_ADDR, and MT_AUTO_RSP_EN is on from init), and the
# header here said "it has never been measured" for some time after it had
# been.
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
# The bring-up tool resolves its firmware directory RELATIVE TO THE WORKING
# DIRECTORY ("firmware/mt7662_rom_patch.bin"), and the symlink below is created
# at $ROOT. Running this script from anywhere else therefore fails the DUT's
# firmware load, which surfaces as "could not read the DUT's MAC" - a message
# that names neither the cause nor the cure. Pin the directory instead.
cd "$ROOT" || exit 1
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
# FW_LINK_OURS: only a link THIS run created is removed afterwards - a
# pre-existing $ROOT/firmware is the operator's.
FW_LINK_OURS=no
if [ ! -e "$ROOT/firmware" ] && ln -sfn "$FW_DIR" "$ROOT/firmware" 2>/dev/null; then
  FW_LINK_OURS=yes
fi

pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

DUT_PID=""
cleanup() {
  [ -n "$DUT_PID" ] && kill "$DUT_PID" 2>/dev/null
  # ...and the one an interrupted arm() left behind in its subshell.
  [ -f "$OUT/.dutpid" ] && kill "$(cat "$OUT/.dutpid")" 2>/dev/null
  rm -f "$OUT/.dutpid"
  # ...and any peer THIS run started that is still holding its USB lock. An
  # orphan here is not this run's problem, it is the next run's: it fails that
  # run's peer open with "adapter already in use", which yields zero reports.
  # -g $$ keeps the sweep inside this run's process group, so a concurrent
  # session's txdemo is not collateral.
  pkill -INT -g $$ -f "$BUILD/txdemo" 2>/dev/null
  [ "$FW_LINK_OURS" = yes ] && rm -f "$ROOT/firmware" && FW_LINK_OURS=no
}
trap cleanup EXIT
# AND IT MUST STOP: with INT/TERM on the EXIT trap the shell runs cleanup
# and then CARRIES ON into the next arm (tests/sta_d2d_onair.sh found and
# fixed this). cleanup is idempotent, so the EXIT pass after it is harmless.
trap 'cleanup; exit 130' INT TERM

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
    # 1 = receiver on, MANAGED filter, nothing armed - the claim.
    # 2 = the same code path with MT_AUTO_RSP_EN cleared - the control.
    # 3 = managed, a WRONG BSSID in an ENABLED APC slot - closes R5.
    #
    # Arm 1 used `bringup arx`, which installs the MONITOR filter at the top
    # of gate_arx, while arm 2 used `norsp`, which leaves the managed one. So
    # the comparison varied the receive filter AND the init path as well as
    # the bit under test, in a write-up that called it single-variable. It is
    # the same defect class as the R5 gate overwriting its own filter, caught
    # in review rather than on the bench. Both arms now run `norsp`, which
    # takes the bit as an argument.
    if [ "$dut_up" = 3 ]; then
      "$BUILD/mt7612uprobe" bssen "$CH" $((SECS + 14)) \
          >"$OUT/dut_$tag.log" 2>&1 &
    elif [ "$dut_up" = 2 ]; then
      "$BUILD/mt7612uprobe" norsp "$CH" $((SECS + 14)) 1 \
          >"$OUT/dut_$tag.log" 2>&1 &
    else
      "$BUILD/mt7612uprobe" norsp "$CH" $((SECS + 14)) 0 \
          >"$OUT/dut_$tag.log" 2>&1 &
    fi
    DUT_PID=$!
    # arm() runs inside a command substitution, so this assignment is invisible
    # to the parent's EXIT trap. Record it where cleanup can find it, or a
    # Ctrl-C mid-arm leaves a bringup holding the adapter - which is exactly
    # the "another process claimed it" cause of a vanished netdev.
    echo "$DUT_PID" > "$OUT/.dutpid"
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

  tx_start=$(date +%s)
  env DEVOURER_VID="$PEER_VID" DEVOURER_PID="$PEER_PID" \
      DEVOURER_USB_BUS="${PEER_SYSFS%%-*}" DEVOURER_USB_PORT="${PEER_SYSFS#*-}" \
      DEVOURER_CHANNEL="$CH" \
      DEVOURER_TX_QOS_DATA=1 DEVOURER_TX_RA="$ra" DEVOURER_TX_SA="$TX_SA" \
      DEVOURER_TX_RATE=MCS3 DEVOURER_TX_PAYLOAD_BYTES=200 \
      DEVOURER_TX_GAP_US=5000 DEVOURER_TX_REPORT=1 \
      DEVOURER_TX_RETRY_LIMIT="$RETRY_LIMIT" \
      DEVOURER_TX_WITH_RX=thread DEVOURER_LOG_LEVEL=warn \
      timeout -s INT -k 3 "$SECS" "$BUILD/txdemo" \
      >"$OUT/tx_$tag.jsonl" 2>"$OUT/tx_$tag.err"
  tx_end=$(date +%s)

  # -k 3 IS LOAD-BEARING. Without it a peer that does not act on SIGINT blocks
  # this arm indefinitely: a `timeout -s INT 12` was found alive SIX MINUTES
  # later, still holding its USB lock, after the run around it was killed. That
  # single orphan then failed the NEXT run's peer open with "adapter already in
  # use", which produced zero reports - and zero is a control's passing value.

  # Did the PEER stay inside its own window? Ask this BEFORE asking anything
  # about the DUT, because a peer that overran is the one explanation under
  # which the DUT's apparent death is not a death at all.
  #
  # The DUT gets SECS+14 s and the peer SECS, so the DUT outlives any peer that
  # behaves. If the peer blocks, the DUT reaches the end of its OWN window and
  # exits NORMALLY - and the liveness check below then reports "the DUT died
  # during the measurement window" while quoting the DUT's own success line as
  # the evidence. That is verbatim what arms A and B printed on the first run
  # of this version: "ABORTED the DUT died DURING ... GATE NORSP: done
  # (restored)". A clean completion is not a death and must not be reported as
  # one; the fault was the peer's, and it is now named as the peer's.
  # SECS+5, not SECS+8: with -k 3 a well-behaved peer is done by SECS+3, and
  # the DUT's own window expires SECS+16 s after ITS launch, i.e. SECS+8 s
  # after the peer started. A threshold of SECS+8 would sit exactly on that
  # boundary and let the misfire back in on a tie.
  if [ $((tx_end - tx_start)) -gt $((SECS + 5)) ]; then
    printf '%s ABORTED the PEER overran its %ss window (took %ss) - nothing about the DUT can be read from this arm' \
           "$tag" "$SECS" "$((tx_end - tx_start))"
    [ -n "$DUT_PID" ] && { kill "$DUT_PID" 2>/dev/null; wait "$DUT_PID" 2>/dev/null; }
    DUT_PID=""; rm -f "$OUT/.dutpid"; return 1
  fi

  # LIVENESS AFTER THE WINDOW, not only before it.
  #
  # The 8 s probe above only proves the arm STARTED. If the DUT wedges at
  # second 9 - a documented failure mode on this part - the peer keeps
  # injecting at an address nobody answers, ok_pct reads ~0, and for a CONTROL
  # arm that is the PASSING value. Arm D would then print "MT_AUTO_RSP_EN is
  # the gate" on the strength of a dead device. Arms A and E fail closed, so
  # this matters for the controls specifically, which is the worse direction.
  if [ -n "$DUT_PID" ] && ! kill -0 "$DUT_PID" 2>/dev/null; then
    printf '%s ABORTED the DUT died DURING the measurement window: %s' \
           "$tag" "$(tail -1 "$OUT/dut_$tag.log" 2>/dev/null)"
    DUT_PID=""; rm -f "$OUT/.dutpid"; return 1
  fi
  [ -n "$DUT_PID" ] && { kill "$DUT_PID" 2>/dev/null; wait "$DUT_PID" 2>/dev/null; }
  DUT_PID=""; rm -f "$OUT/.dutpid"

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

[ "$FW_LINK_OURS" = yes ] && rm -f "$ROOT/firmware" && FW_LINK_OURS=no

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
