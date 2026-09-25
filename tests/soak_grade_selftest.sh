#!/bin/bash
# Headless mutation sweep for the station soak's graders
# (`tests/sta_d2d_onair.sh soak-grade DIR`: the station's books, the AP's
# books, the per-direction trend). No radio, no root.
#
# The fixture (tests/fixtures/soak_grade/) is the ledger lines of a real
# 4-minute bidirectional soak - 8812CU AP, MT7612U station, ch36, 4+4 Mbit/s,
# 5/5. Every case below is a doctored copy with a known answer. K0 must pass;
# M1..M17 each break one thing the graders exist to catch and must FAIL with
# the named message, except M6/M7/M16, which must still pass. M9..M17 are the
# survivors the Phase 5 close-out review found in the first sweep.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
HARNESS="$HERE/sta_d2d_onair.sh"
SRC="$HERE/fixtures/soak_grade"
M="$(mktemp -d)"
trap 'rm -rf "$M"' EXIT
survivors=0

mk() { mkdir -p "$M/$1"; cp "$SRC/ap.log" "$SRC/sta.log" "$SRC/soak.chunks" "$M/$1/"; }
# $1 case, $2 expected rc (0 pass, 1 fail), $3 expected FAIL text or "", env...
check() {
  local name=$1 want=$2 pat=$3; shift 3
  local out rc
  out=$(env SOAK_MINUTES=4 "$@" bash "$HARNESS" soak-grade "$M/$name" 2>&1); rc=$?
  if [ "$rc" = "$want" ] && { [ -z "$pat" ] || grep -qF "FAIL  soak: $pat" <<<"$out"; }; then
    echo "ok       $name"
  else
    echo "SURVIVED $name (rc=$rc, wanted $want '$pat')"; echo "$out" | sed 's/^/    /'
    survivors=$((survivors + 1))
  fi
}
# Rewrite every `<prefix><int>` in $1 through an integer expression of v.
edit() {
  python3 - "$1" "$2" "$3" <<'EOF'
import re, sys
p, rx, expr = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
s = re.sub(rx, lambda m: m.group(1) + str(eval(expr, {'v': int(m.group(2))})), s)
open(p, 'w').write(s)
EOF
}

mk K0_known_good;                 check K0_known_good 0 ""
mk M1_ap_send_failed_plus1
edit "$M/M1_ap_send_failed_plus1/ap.log" '(send failed=)(\d+)' 'v+1'
check M1_ap_send_failed_plus1 1 "the AP's books do NOT close"
mk M2_ap_framed_minus1
edit "$M/M2_ap_framed_minus1/ap.log" '(framed=)(\d+)' 'v-1'
check M2_ap_framed_minus1 1 "the AP's books do NOT close"
mk M3_down_last_quarter_halved
awk 'NR==4{$4=$4/2} {print}' "$SRC/soak.chunks" >"$M/M3_down_last_quarter_halved/soak.chunks"
check M3_down_last_quarter_halved 1 "throughput DEGRADED"
mk M4_up_last_quarter_halved
awk 'NR==4{$2=$2/2} {print}' "$SRC/soak.chunks" >"$M/M4_up_last_quarter_halved/soak.chunks"
check M4_up_last_quarter_halved 1 "throughput DEGRADED"
mk M5_down_dead_throughout
awk '{$4=0} {print}' "$SRC/soak.chunks" >"$M/M5_down_dead_throughout/soak.chunks"
check M5_down_dead_throughout 1 "throughput DEGRADED"
mk M6_down_dead_but_not_loaded
awk '{$4=0} {print}' "$SRC/soak.chunks" >"$M/M6_down_dead_but_not_loaded/soak.chunks"
check M6_down_dead_but_not_loaded 0 "" SOAK_DOWN_KBIT=0
mk M7_down_within_allowance
awk 'NR==4{$4=$4*0.9} {print}' "$SRC/soak.chunks" >"$M/M7_down_within_allowance/soak.chunks"
check M7_down_within_allowance 0 ""
mk M8_ap_log_empty
: >"$M/M8_ap_log_empty/ap.log"
check M8_ap_log_empty 1 "the AP's books do NOT close"
mk M9_ap_log_tap_line_only
grep -F "TAP:" "$SRC/ap.log" >"$M/M9_ap_log_tap_line_only/ap.log"
check M9_ap_log_tap_line_only 1 "the AP's books do NOT close"
mk M10_one_chunk
head -1 "$SRC/soak.chunks" >"$M/M10_one_chunk/soak.chunks"
check M10_one_chunk 1 "only 1 chunk"
mk M11_three_chunks
head -3 "$SRC/soak.chunks" >"$M/M11_three_chunks/soak.chunks"
check M11_three_chunks 1 "only 3 chunk"
mk M12_degrade_pct_1000
awk 'NR==4{$4=$4/2} {print}' "$SRC/soak.chunks" >"$M/M12_degrade_pct_1000/soak.chunks"
check M12_degrade_pct_1000 1 "SOAK_DOWN_KBIT and SOAK_DEGRADE_PCT must be" SOAK_DEGRADE_PCT=1000
mk M13_sta_log_empty
: >"$M/M13_sta_log_empty/sta.log"
check M13_sta_log_empty 1 "the station's books do NOT close"
mk M14_sta_aired_minus1
edit "$M/M14_sta_aired_minus1/sta.log" '(aired=)(\d+)' 'v-1'
check M14_sta_aired_minus1 1 "the station's books do NOT close"
mk M15_breaker_tripped_books_close
edit "$M/M15_breaker_tripped_books_close/ap.log" '(refused after the TX circuit opened=)(\d+)' 'v+5'
edit "$M/M15_breaker_tripped_books_close/ap.log" '(queued=)(\d+)' 'v+5'
check M15_breaker_tripped_books_close 1 "the AP's books close but its TX circuit breaker OPENED"
mk M16_watchdog_queued_line_after
printf '  TX-DMA watchdog: 0x0 -> 0x40000 after queued=1 sent=1 send_failed=0 qdrop=0\n' \
    >>"$M/M16_watchdog_queued_line_after/ap.log"
check M16_watchdog_queued_line_after 0 ""
mk M17_sta_zero_traffic
edit "$M/M17_sta_zero_traffic/sta.log" '(from host=)(\d+)' '0'
edit "$M/M17_sta_zero_traffic/sta.log" '(tx: encrypted=)(\d+)' '0'
check M17_sta_zero_traffic 1 "the station's books do NOT close"

echo "survivors: $survivors (18 cases)"
[ "$survivors" = 0 ]
