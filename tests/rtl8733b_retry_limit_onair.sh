#!/usr/bin/env bash
# rtl8733b_retry_limit_onair.sh — is DeviceConfig::tx::retry_limit a LIVE
# actuator on the RTL8733B, or just an encoded descriptor field?
#
# The 8733B has no CCX / tx.report path (the firmware emits no C2H reports —
# src/rtl8733b/CLAUDE.md), so the TX side cannot be its own witness the way
# tests/ack_txreport_matrix.sh judges the Jaguars. This bench judges from the
# AIR instead: the DUT sends unicast QoS-Data to a MAC that nobody owns, so no
# ACK ever comes back and the MAC must exhaust its descriptor retry limit on
# every frame. A passive monitor counts how many times each submitted frame
# actually aired.
#
#   retry_limit = N  ->  airings/frame ~= 1 + N   (1 original + N retries)
#
# That is a DOSE-RESPONSE, not an A/B: a single on/off pair could be explained
# by ambient conditions, three or more levels on a straight line cannot.
#
# COUNTING NOTE: rxdemo emits rx.txhit sampled (first 10, then every 100th) —
# counting EVENTS undercounts by 100x. The event's own `hits` field is the
# cumulative truth, so we read the LAST hits value. That quantizes the total to
# the largest multiple of 100 <= H, i.e. H is understated by up to 99 airings
# (at the FRAMES_MIN floor below, <=0.099 airings/frame). Each arm gets a FRESH
# witness
# so `hits` starts at zero — the counter is static for the process lifetime and
# does not reset between arms.
#
#   sudo bash tests/rtl8733b_retry_limit_onair.sh
#   ARMS="0 3 12" FRAMES=3000 CH=36 sudo bash tests/rtl8733b_retry_limit_onair.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=${BUILD:-$ROOT/build}

DUT_VID=${DUT_VID:-0x0bda}; DUT_PID=${DUT_PID:-0xf72b}   # RTL8731BU/8733BU
WIT_VID=${WIT_VID:-0x0bda}; WIT_PID=${WIT_PID:-0x8812}   # RTL8812AU witness
CH=${CH:-6}
ARMS=${ARMS:-"0 3 12 0 12"}
FRAMES=${FRAMES:-1500}
RATE=${RATE:-MCS0}
GAP_US=${GAP_US:-3000}
PWR_QDB=${PWR_QDB:-12}
# RA must be UNICAST (so an ACK is expected) and unowned (so none ever comes).
RA=${RA:-02:de:ad:be:ef:01}
OUT=${OUT:-/tmp/rtl8733b_retry}

# The readout is quantized to the last rx.txhit checkpoint (see COUNTING NOTE),
# so the error is a fixed <=99 airings whatever FRAMES is — which only stays
# negligible while FRAMES is large. At FRAMES=60 a healthy retry=0 arm airs ~55
# times, the last checkpoint reads hits=10, and the arm scores 0.17 against a
# 0.6 floor: a FAIL on working hardware. Refuse below a floor rather than
# report that, for the same reason an arm that did not run is refused instead
# of reported as zero.
FRAMES_MIN=${FRAMES_MIN:-1000}
if [ "$FRAMES" -lt "$FRAMES_MIN" ]; then
  echo "ABORT: FRAMES=$FRAMES is below FRAMES_MIN=$FRAMES_MIN — the rx.txhit" >&2
  echo "       readout quantizes to <=99 airings, which at this size is a" >&2
  echo "       false FAIL on healthy hardware, not a measurement." >&2
  exit 2
fi

KILL(){ sudo pkill -9 -x rxdemo 2>/dev/null; sudo pkill -9 -x txdemo 2>/dev/null; return 0; }
trap KILL EXIT
mkdir -p "$OUT"; RESULTS="$OUT/results.jsonl"; : >"$RESULTS"

idx=0
for arm in $ARMS; do
  idx=$((idx+1))
  # Per-ARM-INDEX filenames, not per-retry-value: a ladder repeats values
  # (0/3/12/0/12) and reusing the value as the name lets a slow-dying witness
  # from the earlier arm append into the next one's log.
  tag="$(printf '%02d_r%s' "$idx" "$arm")"
  KILL; sleep 3   # USB release after a -9 is not instantaneous
  sudo env DEVOURER_VID=$WIT_VID DEVOURER_PID=$WIT_PID DEVOURER_CHANNEL=$CH \
       DEVOURER_LOG_LEVEL=info \
       "$BUILD/rxdemo" >"$OUT/wit_$tag.jsonl" 2>"$OUT/wit_$tag.err" &
  waited=0
  until grep -qE "async ring of .* URBs submitted|Listening air" "$OUT/wit_$tag.err"; do
    sleep 1; waited=$((waited+1))
    if [ "$waited" -ge 25 ]; then
      echo "ABORT: witness never reached RX for arm=$arm (#$idx)" >&2
      tail -5 "$OUT/wit_$tag.err" >&2; exit 1
    fi
  done
  sleep 2
  sudo env DEVOURER_VID=$DUT_VID DEVOURER_PID=$DUT_PID DEVOURER_CHANNEL=$CH \
       DEVOURER_TX_QOS_DATA=1 DEVOURER_TX_RA=$RA \
       DEVOURER_TX_RATE=$RATE DEVOURER_TX_PAYLOAD_BYTES=200 \
       DEVOURER_TX_GAP_US=$GAP_US DEVOURER_TX_FRAMES=$FRAMES \
       DEVOURER_TX_RETRY_LIMIT=$arm DEVOURER_TX_PWR_OFFSET_QDB=$PWR_QDB \
       DEVOURER_LOG_LEVEL=warn \
       timeout -s INT 90 "$BUILD/txdemo" >"$OUT/tx_$tag.jsonl" 2>"$OUT/tx_$tag.err" || true
  sleep 3
  sent=$(grep '"ev":"tx.stats"' "$OUT/tx_$tag.jsonl" | tail -1 |
         sed -n 's/.*"submitted":\([0-9]*\).*/\1/p'); sent=${sent:-0}
  hits=$(grep -o '"ev":"rx.txhit","hits":[0-9]*' "$OUT/wit_$tag.jsonl" | tail -1 |
         sed -n 's/.*"hits":\([0-9]*\).*/\1/p'); hits=${hits:-0}
  KILL
  # A cell that did not run is NOT a measurement of zero. An arm whose DUT
  # never opened (sent=0) or whose witness heard nothing at all (hits=0) is a
  # harness failure, and reporting it as 0.0 airings/frame would read exactly
  # like a dead retry engine — the same false-verdict shape that made an
  # 8821AU look broken in tests/ack_txreport_matrix.sh. Abort instead.
  if [ "$sent" -eq 0 ] || [ "$hits" -eq 0 ]; then
    echo "ABORT: arm=$arm (#$idx) did not run — submitted=$sent airings=$hits" >&2
    echo "       (DUT or witness failed to open; this is not a zero result)" >&2
    tail -5 "$OUT/tx_$tag.err" >&2
    exit 1
  fi
  if [ "$sent" -ne "$FRAMES" ]; then
    echo "WARN: arm=$arm (#$idx) submitted $sent of $FRAMES requested" >&2
  fi
  python3 - "$arm" "$sent" "$hits" >>"$RESULTS" <<'PY'
import json, sys
arm, sent, hits = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
per = round(hits / sent, 2) if sent else 0.0
print(json.dumps({"ev": "retry.arm", "retry_limit": arm, "submitted": sent,
                  "airings": hits, "airings_per_frame": per,
                  "expected": 1 + arm}))
PY
  tail -1 "$RESULTS"
done

echo "==== VERDICT ===="
python3 - "$RESULTS" <<'PY'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
ok = True
for r in rows:
    exp, got = r["expected"], r["airings_per_frame"]
    # Generous band: airings can only be LOST (a monitor misses frames), never
    # invented, so the floor is what matters. 0.6*expected still separates
    # every adjacent level in a 0/3/12 ladder.
    good = 0.6 * exp <= got <= 1.15 * exp
    ok &= good
    print(f"  retry={r['retry_limit']:>2}  expected~{exp:>2}  measured={got:>5}"
          f"  {'OK' if good else 'FAIL'}")
print(json.dumps({"ev": "retry.verdict", "tx_retry_limit_ok": bool(ok),
                  "arms": len(rows)}))
sys.exit(0 if ok else 1)
PY
