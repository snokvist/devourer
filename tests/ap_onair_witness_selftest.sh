#!/bin/bash
# ap_onair_witness_selftest.sh - headless guard for the two load-bearing
# verdict functions inside tests/mt7612u_ap_onair.sh.
#
# Those two decide whether the AP acceptance gate passes, and BOTH replaced a
# check that could not fail:
#
#   - the auto-ACK witness. The old form grepped the AP's own log for a
#     management frame logged at retry=0. ap_responder logs every received
#     copy, and reception does not depend on ACKing, so an AP with its ACK
#     responder disarmed still produced that line and still passed. The
#     replacement reads mac80211's per-peer tx_retries / tx_failed from the
#     station, which is the only side that knows whether its frames were
#     acknowledged.
#
#   - the "beacon is gone" witness, which is the only air evidence this suite
#     has that StopBeacon works. Absence checks pass on zero, so a scan that
#     errored, never ran, or failed to parse looked exactly like a beacon that
#     really stopped - and a beacon filed by cfg80211 under a frequency we did
#     not scan looked the same again. Both are observed on the bench.
#
# The parsing is pure text, so it is testable with no hardware and belongs in
# ctest. Run directly, or via `ctest -R ap_onair_witness`.
#
# This file must keep the awk and the verdict ladder BYTE-EQUIVALENT to
# tests/mt7612u_ap_onair.sh. They are duplicated because that script needs a
# live interface; if you change one, change both.
set -u
fails=0
chk() { # $1 = expectation, $2 = got, $3 = label
  if [ "$1" = "$2" ]; then printf '  ok    %-52s -> %s\n' "$3" "$2"
  else printf '  BROKE %-52s -> got %s want %s\n' "$3" "$2" "$1"; fails=$((fails+1)); fi
}

# ---- 1. the auto-ACK witness: parse + verdict -----------------------------
verdict() { # stdin = `iw station dump` text
  local sta_tx tx_pkts tx_retries tx_failed
  sta_tx=$(awk '/tx packets:/ {p=$3} /tx retries:/ {r=$3} /tx failed:/ {f=$3}
                END { print (p=="" ? "-" : p), (r=="" ? "-" : r), (f=="" ? "-" : f) }')
  set -- $sta_tx
  tx_pkts="${1:--}"; tx_retries="${2:--}"; tx_failed="${3:--}"
  if [ "$tx_pkts" = "-" ] || [ "$tx_retries" = "-" ] || [ "$tx_failed" = "-" ]; then
    echo NO_WITNESS
  elif [ "$tx_pkts" -le 0 ]; then echo NO_FRAMES
  elif [ "$tx_failed" -gt 0 ]; then echo NOT_ACKING
  elif [ "$tx_retries" -gt "$tx_pkts" ]; then echo BARELY_ACKING
  else echo PASS; fi
}

healthy=$'\ttx packets:\t48\n\ttx retries:\t0\n\ttx failed:\t0'
chk PASS          "$(verdict <<<"$healthy")"                   "healthy link (48 frames, 0 retries)"
# the defect the check exists to catch: AP never ACKs -> MAC exhausts the
# ladder on every frame and gives up
deadack=$'\ttx packets:\t48\n\ttx retries:\t763\n\ttx failed:\t41'
chk NOT_ACKING    "$(verdict <<<"$deadack")"                   "AP not ACKing (tx failed > 0)"
# a marginal responder: nothing outright fails but every frame is retried
marginal=$'\ttx packets:\t48\n\ttx retries:\t512\n\ttx failed:\t0'
chk BARELY_ACKING "$(verdict <<<"$marginal")"                  "AP barely ACKing (retries >> frames)"
chk NO_WITNESS    "$(verdict <<<'')"                           "station exports no counters"
chk NO_WITNESS    "$(verdict <<<$'\ttx packets:\t48')"         "counters only partly exported"
chk NO_FRAMES     "$(verdict <<<$'\ttx packets:\t0\n\ttx retries:\t0\n\ttx failed:\t0')" "station transmitted nothing"

# ---- 2. seen()/gone(): the positive control -------------------------------
count() { # $1 = ssid  $2 = bssid  $3 = freq|any ; stdin = scan text
  awk -v b="$2" -v ss="$1" -v want="$3" '
    /^BSS / { k++; bssid[k] = tolower($2); sub(/\(.*/, "", bssid[k]); next }
    k > 0 && $1 == "freq:" { f[k] = $2 + 0; next }
    k > 0 { line = $0; sub(/^[ \t]+/, "", line); sub(/[ \t\r]+$/, "", line)
            if (line == "SSID: " ss) sname[k] = 1 }
    END { for (j = 1; j <= k; j++)
            if (bssid[j] == tolower(b) && sname[j] &&
                (want == "any" || f[j] == want + 0)) c++
          print (c + 0) " " (k + 0) }'
}
gone_verdict() { # stdin = scan text
  local r m b; r=$(count "$@"); m=${r%% *}; b=${r##* }
  if [ "$b" -eq 0 ]; then echo SCAN_DEAD
  elif [ "$m" -eq 0 ]; then echo GONE
  else echo STILL_AIRING; fi
}

ours=$'BSS 02:42:75:05:d6:00(on wlan0)\n\tfreq: 5180.0\n\tSSID: devourerAP'
ghost=$'BSS 02:42:75:05:d6:00(on wlan0)\n\tfreq: 2412.0\n\tSSID: devourerAP'
nbr=$'BSS 70:3a:cb:f2:ce:2d(on wlan0)\n\tfreq: 5180.0\n\tSSID: Trollvinter'

chk "1 1"  "$(count devourerAP 02:42:75:05:d6:00 5180 <<<"$ours")"  "airing: our beacon at the scanned freq"
chk "0 1"  "$(count devourerAP 02:42:75:05:d6:00 5180 <<<"$ghost")" "airing: pinned to freq, ghost excluded"
# THE REGRESSION THIS EXISTS TO CATCH: a beacon still airing but filed under
# another frequency. A freq-pinned "gone" check would call this silenced.
chk STILL_AIRING "$(gone_verdict devourerAP 02:42:75:05:d6:00 any <<<"$ghost")" \
     "gone: still-airing beacon on a wrong freq is NOT gone"
chk GONE         "$(gone_verdict devourerAP 02:42:75:05:d6:00 any <<<"$nbr")" \
     "gone: genuinely absent, with the scan proven alive"
# THE OTHER REGRESSION: a scan that never ran must not read as silence.
chk SCAN_DEAD    "$(gone_verdict devourerAP 02:42:75:05:d6:00 any <<<'')" \
     "gone: dead scan refused, not counted as gone"
chk SCAN_DEAD    "$(gone_verdict devourerAP 02:42:75:05:d6:00 any <<<'command failed: Device or resource busy (-16)')" \
     "gone: busy-interface error refused"
# exact SSID match, not substring
chk "0 1"  "$(count devourerAP 02:42:75:05:d6:00 5180 <<<$'BSS 02:42:75:05:d6:00(on wlan0)\n\tfreq: 5180.0\n\tSSID: devourerAP2')" \
     "airing: devourerAP2 does not match devourerAP"
# trailing whitespace on the SSID line used to defeat the exact compare
chk "1 1"  "$(count devourerAP 02:42:75:05:d6:00 5180 <<<$'BSS 02:42:75:05:d6:00(on wlan0)\n\tfreq: 5180.0\n\tSSID: devourerAP  ')" \
     "airing: trailing whitespace tolerated"

echo
[ "$fails" = 0 ] && echo "=== all witnesses can distinguish pass from fail ===" \
                 || echo "=== $fails BROKEN ==="
exit $(( fails > 0 ))
