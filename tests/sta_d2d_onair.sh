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
#   thru      one-way UDP goodput, both directions - the first real throughput
#   beacons   which end starves the beacon, with no third radio needed
#   soak      a long run, both directions loaded (SOAK_KBIT up, SOAK_DOWN_KBIT
#             down): does the link hold, do both ledgers still close
#   bench     software CCMP cost at BOTH ends, under a load the link sustains
#   flood     the ceiling, and whether both ledgers account for what exceeds it
#
#   soak-grade DIR   offline, no radio, no root: re-run the soak's ledger and
#             trend graders over a saved $OUT - how they are mutation-tested
#
#   sudo tests/sta_d2d_onair.sh                # wpa2 + fiveghz + airgap
#   sudo CH=36 SOAK_MINUTES=30 tests/sta_d2d_onair.sh soak
#   sudo CH=6 tests/sta_d2d_onair.sh wpa2
#   sudo CH5=149 tests/sta_d2d_onair.sh fiveghz
#   sudo tests/sta_d2d_onair.sh bench
#   sudo tests/sta_d2d_onair.sh flood
#
# Env: STA_SYSFS (the MT7612U sta_client claims), AP_SYSFS (the adapter
# ap_wpa2 claims), AP_VID/AP_PID, CH, CH5, PSK, FW_DIR, SECS, NS, APTAP,
# STATAP, AIRGAP_SECS, BEACONS_MIN, PING_N, PING_MIN, BENCH_SECS,
# BENCH_PAYLOAD, BENCH_PPS, THRU_SECS, THRU_PAYLOAD, TX_RATE, ARQ,
# THRU_LADDER, THRU_LOSS_PCT, THRU_DIR, BCN_TU, BEACON_PHASE_SECS,
# BEACON_KBIT, BEACON_FLOOR_PCT, BCN_REFRESH_MS, SOAK_MINUTES, SOAK_KBIT,
# SOAK_DOWN_KBIT, SOAK_CHUNK_S, SOAK_DEGRADE_PCT.

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
# The falsifier's own control: how many beacons the station must have heard
# from ANYONE to prove its receiver was awake while it found nothing of ours.
# This bench's ch6 carries ~40 a second.
BEACONS_MIN="${BEACONS_MIN:-20}"
# The data-plane ping: how many, and how many must arrive. See the note in
# ping_cell for why this is a majority over twenty rather than zero loss
# over six.
PING_N="${PING_N:-20}"
PING_MIN="${PING_MIN:-16}"
# The throughput cell: how long each direction runs, and how big the UDP
# payload is. 1400 avoids IP fragmentation over a 1500-byte TAP.
THRU_SECS="${THRU_SECS:-15}"
THRU_PAYLOAD="${THRU_PAYLOAD:-1400}"
# The offered-rate ladder, in kbit/s, and the loss a rung may carry and
# still count. 5% is loose, and deliberately so: neither end retransmits
# (see ARQ), so the knee of a link like this is where loss climbs, not
# where it appears.
THRU_LADDER="${THRU_LADDER:-1000,2000,4000,6000,9000,14000,20000,30000}"
THRU_LOSS_PCT="${THRU_LOSS_PCT:-5}"
# THE TWO PERFORMANCE KNOBS, both off by default so every figure already
# recorded stays reproducible.
#
# TX_RATE is the rate EVERY frame airs at, at both ends. The default is 6M
# legacy - the most robust OFDM rate there is, and a 6 Mbit/s ceiling. There
# is no rate control anywhere in this project's station path and there is not
# going to be: choosing a rate from link statistics is the integrator's job.
# What the harness owes is the ability to ask. Grammar is DEVOURER_TX_RATE's
# (see CLAUDE.md): 6M, MCS7/40/SGI, VHT2SS_MCS3/80/LDPC, ...
TX_RATE="${TX_RATE:-6M}"
# ARQ=1 arms the AP's hardware ACK responder on the BSSID, which closes a
# MAC-level retransmission loop for the STATION'S UPLINK: the station's MAC
# retries until the AP acknowledges. The station side needs nothing - an
# MT7612U auto-ACKs its own address from SetStationIdentity, measured at
# 99.9% in Phase 0 (docs/mt7612u-station-identity.md, R6).
#
# NOT symmetric, and the asymmetry is the point: this arms the AP's RECEIVER
# to answer, and whether the AP's TRANSMITTER asks for an ACK is a descriptor
# property this harness does not set. So expect the uplink to improve and
# make no prediction about the downlink - measure it.
ARQ="${ARQ:-0}"
# The AP's beacon interval, in TU. 25 is what every figure in this branch was
# measured under and stays the default; 100 is what a normal AP uses. It is a
# knob because the beacon rides the same chip as the data queue and "does the
# beacon starve the data path" is a question with a one-word answer only if
# you can change it.
BCN_TU="${BCN_TU:-25}"
# The beacon keepalive: how often the AP re-downloads its beacon page, in
# milliseconds. 0 is off and is the default, because every figure already
# recorded in this branch was taken without it. See the note in
# tests/ap_wpa2.cpp for why the vendor driver does this on USB.
BCN_REFRESH_MS="${BCN_REFRESH_MS:-0}"
# Override the AP's TX descriptor QSEL. Unset leaves the backend default,
# which on Jaguar3 is 0x12 (MGNT) for EVERY frame - the thing under
# investigation. 0 is TID0/BE. A diagnostic knob, not a setting.
AP_TX_QSEL="${AP_TX_QSEL:-}"
# Which direction the throughput ladder walks. `both` is the measurement;
# `up` and `down` exist so a diagnostic arm does not have to pay for the half
# it is not asking about - which on a shared bench is most of the cost.
THRU_DIR="${THRU_DIR:-both}"
# The beacons cell: how long each of its three phases runs, the offered
# rate it loads the link with, and how far beacon reception may fall and
# still count as surviving.
BEACON_PHASE_SECS="${BEACON_PHASE_SECS:-12}"
BEACON_KBIT="${BEACON_KBIT:-4000}"
BEACON_FLOOR_PCT="${BEACON_FLOOR_PCT:-50}"
# The soak. Bidirectional: SOAK_KBIT up and SOAK_DOWN_KBIT down, at the same
# time. SOAK_DOWN_KBIT=0 gives back the uplink-only soak that was all the
# AP could take before the Jaguar3/Jaguar2 TX-ring fix.
SOAK_MINUTES="${SOAK_MINUTES:-30}"
SOAK_KBIT="${SOAK_KBIT:-4000}"
SOAK_DOWN_KBIT="${SOAK_DOWN_KBIT:-4000}"
SOAK_CHUNK_S="${SOAK_CHUNK_S:-60}"
SOAK_DEGRADE_PCT="${SOAK_DEGRADE_PCT:-20}"
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

[ "$CELLS" = soak-grade ] || [ "$(id -u)" = 0 ] || { echo "must run as root"; exit 2; }
pass=0; fail=0
say() { printf '%s\n' "$*"; }
ok()  { pass=$((pass+1)); printf '  PASS  %s\n' "$*"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n' "$*"; }

# The soak's two ledger-and-trend graders, over the files a soak leaves in
# $OUT (ap.log, soak.chunks). A function, and defined up here, so that
# `soak-grade DIR` can run them offline over a saved or doctored run - which
# is how they are mutation-tested without spending thirty minutes of air.
soak_grade_tail() {
  local chunks; chunks=$(wc -l <"$OUT/soak.chunks" 2>/dev/null)
  apl() { sed -n "s/.*$1=\\([0-9][0-9]*\\).*/\\1/p" "$OUT/ap.log" | tail -1; }
  # -- the AP's books, the flood cell's identity: every host frame framed or
  # refused, every queued frame aired, queue-dropped or send-failed
  local a_from a_framed a_down a_q a_sent a_qdrop a_sfail
  a_from=$(apl 'from host'); a_framed=$(apl 'framed')
  a_down=$(apl 'dropped down'); a_q=$(apl 'queued')
  a_sent=$(apl 'frames sent'); a_qdrop=$(apl 'queue dropped')
  a_sfail=$(apl 'send failed')
  if [ "${a_from:-0}" -gt 0 ] 2>/dev/null &&
     [ $(( ${a_framed:-0} + ${a_down:-0} )) = "${a_from:-0}" ] &&
     [ $(( ${a_sent:-0} + ${a_qdrop:-0} + ${a_sfail:-0} )) = "${a_q:-0}" ]; then
    ok "soak: the AP's books still close after ${SOAK_MINUTES} min ($a_from host frames, $a_q queued, $a_sent aired, $a_sfail send-failed)"
  else
    bad "soak: the AP's books DRIFTED over the run - host: $a_from vs $a_framed+$a_down; queue: $a_q vs $a_sent+$a_qdrop+$a_sfail"
  fi

  # -- degradation: last quarter against first, per loaded direction
  local q first last dfirst dlast
  q=$(( ${chunks:-0} / 4 )); [ "$q" -lt 1 ] && q=1
  first=$(head -n "$q" "$OUT/soak.chunks" | awk '{s+=$2} END{printf "%.3f", s/NR}')
  last=$(tail -n "$q" "$OUT/soak.chunks" | awk '{s+=$2} END{printf "%.3f", s/NR}')
  dfirst=$(head -n "$q" "$OUT/soak.chunks" | awk '{s+=$4} END{printf "%.3f", s/NR}')
  dlast=$(tail -n "$q" "$OUT/soak.chunks" | awk '{s+=$4} END{printf "%.3f", s/NR}')
  local up_ok=1 dn_ok=1
  awk -v f="$first" -v l="$last" -v p="$SOAK_DEGRADE_PCT" \
      'BEGIN{exit !(f>0 && 100*l/f >= 100-p)}' || up_ok=0
  if [ "$SOAK_DOWN_KBIT" -gt 0 ]; then
    awk -v f="$dfirst" -v l="$dlast" -v p="$SOAK_DEGRADE_PCT" \
        'BEGIN{exit !(f>0 && 100*l/f >= 100-p)}' || dn_ok=0
  fi
  if [ "$up_ok$dn_ok" = 11 ]; then
    ok "soak: no throughput degradation - up ${first} -> ${last} Mbit/s, down ${dfirst} -> ${dlast} Mbit/s (first quarter -> last)"
  else
    bad "soak: throughput DEGRADED over the run - up ${first} -> ${last}, down ${dfirst} -> ${dlast} Mbit/s (allowed ${SOAK_DEGRADE_PCT}%)"
  fi
}

# `soak-grade DIR`: grade a saved soak directory offline. No radio, no root.
if [ "$CELLS" = soak-grade ]; then
  OUT="${2:?usage: $0 soak-grade DIR}"
  soak_grade_tail
  say "=== $pass passed, $fail failed (offline grade of $OUT) ==="
  [ $((pass + fail)) = 2 ] || { say "=== HARNESS ERROR: expected 2 checks ==="; exit 2; }
  exit $(( fail > 0 ))
fi
mkdir -p "$OUT"

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
NS_OURS=no

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
  # ONLY A NAMESPACE THIS RUN CREATED. Deleting it is safe in a way it is NOT
  # in mt7612u_sta_onair.sh - nothing but a TAP ever enters this one, because
  # devourer holds both radios over libusb and neither has a phy to move -
  # but "safe to delete" is not the same as "ours to delete", and this used
  # to remove any namespace that happened to carry the name.
  if [ "$NS_OURS" = yes ]; then
    ip netns del "$NS" 2>/dev/null
    NS_OURS=no
  fi
  return 0
}
trap cleanup EXIT
# AND IT MUST STOP. With INT and TERM on the same trap as EXIT, bash runs
# cleanup and then CARRIES ON: Ctrl-C during a cell killed both endpoints, the
# cell reported "an endpoint exited", and the case statement went straight
# into bringing the next one up. Found by review.
trap 'cleanup; exit 130' INT TERM

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
say "tx rate '$TX_RATE'  arq $ARQ  beacon ${BCN_TU}TU  refresh ${BCN_REFRESH_MS}ms"

# --- build -----------------------------------------------------------------

build_both() {
  [ -f "$BUILD/libdevourer.a" ] || {
    say "no $BUILD/libdevourer.a - build the library first"; return 1; }
  local cf; cf=$(pkg-config --cflags --libs libusb-1.0) || return 1
  g++ -std=c++20 -O2 "$ROOT/tests/udp_blast.cpp" -o "$OUT/udp_blast" || return 1
  local f
  for f in ap_wpa2 sta_client; do
    g++ -std=c++20 -O2 -I"$ROOT/src" -I"$ROOT/tests" -I"$ROOT/examples/common" \
        "$ROOT/tests/$f.cpp" "$ROOT/examples/common/env_config.cpp" \
        "$ROOT/examples/common/usb_select.cpp" "$BUILD/libdevourer.a" \
        $cf -lcrypto -lpthread -o "$OUT/$f" || return 1
  done
}

# --- the AP ----------------------------------------------------------------

# WE CREATE THE NAMESPACE OR WE REFUSE IT. This used to return success for
# any pre-existing netns of the same name, and cleanup() then deleted it
# unconditionally - so a run with NS set to something the operator was using
# would take it over and then destroy it. A leftover of OUR OWN is safe to
# remove, because nothing but a TAP ever goes in here; anything else is not
# ours to judge.
ns_up() {
  if ip netns list 2>/dev/null | grep -q "^$NS"; then
    local ifs
    ifs=$(ip netns exec "$NS" ip -br link show 2>/dev/null |
          grep -cv '^lo ') || ifs=0
    if [ "${ifs:-0}" -gt 1 ]; then
      say "netns $NS already exists and is NOT empty - refusing to use or delete it."
      say "  Set NS=<other name>, or remove it yourself if it is a leftover."
      return 1
    fi
    ip netns del "$NS" 2>/dev/null
  fi
  ip netns add "$NS" || return 1
  NS_OURS=yes
}

ap_up() {   # $1 = channel, $2 = seconds, $3.. = extra env
  local chan="$1" secs="$2"; shift 2
  rm -f "$OUT/ap.log"
  local arq_env=""
  [ "$ARQ" = 1 ] && arq_env="DEVOURER_ACK_RESPONDER=$BSSID"
  ip netns exec "$NS" env \
      ${arq_env:+"$arq_env"} DEVOURER_TX_RATE="$TX_RATE" \
      ${AP_TX_QSEL:+DEVOURER_TX_QSEL="$AP_TX_QSEL"} \
      DEVOURER_AP_BCN_REFRESH_MS="$BCN_REFRESH_MS" \
      DEVOURER_VID="$AP_VID" DEVOURER_PID="$AP_PID" \
      DEVOURER_USB_BUS="${AP_SYSFS%%-*}" DEVOURER_USB_PORT="${AP_SYSFS#*-}" \
      DEVOURER_CHANNEL="$chan" DEVOURER_WPA2_PSK="$PSK" DEVOURER_BCN_TU="$BCN_TU" \
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
  env DEVOURER_TX_RATE="$TX_RATE" \
      DEVOURER_VID=0x0e8d DEVOURER_PID=0x7612 \
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
  # Not restored afterwards, and it does not need to be: the TAP is
   # sta_client's and ceases to exist when it exits, so there is no device
   # left for NetworkManager to manage. (The variable that used to record
   # this was never read - a dead assignment the review caught.)
  command -v nmcli >/dev/null 2>&1 &&
    nmcli device set "$STATAP" managed no >/dev/null 2>&1 || true
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
    ping -c "$PING_N" -i 0.2 -W 2 -I "$STATAP" "$APIP" >"$OUT/$cell.$dir.ping" 2>&1
  else
    ip netns exec "$NS" ping -c 1 -W 3 -I "$APTAP" "$STAIP" >/dev/null 2>&1
    ip netns exec "$NS" ping -c "$PING_N" -i 0.2 -W 2 -I "$APTAP" "$STAIP" \
        >"$OUT/$cell.$dir.ping" 2>&1
  fi
  if ! sta_alive || ! ap_alive; then
    bad "$cell: $label - an endpoint exited DURING the measurement (raise SECS, currently $SECS)"
    return 1
  fi
  local p_rx p_loss
  p_rx=$(sed -n 's/.* \([0-9][0-9]*\) received.*/\1/p' "$OUT/$cell.$dir.ping" | tail -1)
  p_loss=$(sed -n 's/.* \([0-9.][0-9.]*%\) packet loss.*/\1/p' "$OUT/$cell.$dir.ping" | tail -1)
  p_rx=${p_rx:-0}; p_loss=${p_loss:-unknown}
  # A MAJORITY, OVER ENOUGH PACKETS TO MEAN SOMETHING - not zero over six.
  #
  # NEITHER END ARMS SetAckResponder, so this link has no link-layer
  # retransmission in either direction and a frame lost to a neighbour's
  # transmission is lost for good. Six packets with a zero-loss threshold
  # made this cell a coin toss on a busy band: it read 0% four times and then
  # 16.7% (one packet of six) and 50% (three of six) on channels that had
  # just carried a clean run. That is the ROOM, and a cell that grades the
  # room is a cell that will be ignored.
  #
  # What the threshold still catches is what the cell is for: a data plane
  # that does not work at all reads 100% loss, and an association that comes
  # up without carrying traffic reads the same. Both fail this, by a mile.
  if [ "$p_rx" -ge "$PING_MIN" ] 2>/dev/null; then
    ok "$cell: $label - $p_rx/$PING_N delivered, loss=$p_loss ($(grep -oE 'rtt [^ ]+ = [0-9./]+' "$OUT/$cell.$dir.ping" | head -1))"
    return 0
  fi
  bad "$cell: $label - only $p_rx/$PING_N delivered (loss=$p_loss, want >= $PING_MIN)"
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
  # A REAL GAP, not merely a different number. CH=36 CH5=40 are adjacent
  # 20 MHz channels whose skirts overlap, and the cell's conclusion - "these
  # radios cannot hear each other" - would then be a guess. 40 MHz apart is
  # the narrowest separation this asserts.
  local gap
  if [ "$freq" -gt "$freq5" ]; then gap=$(( freq - freq5 )); else gap=$(( freq5 - freq )); fi
  [ "$gap" -ge 40 ] || {
    bad "airgap: ch$CH and ch$CH5 are only ${gap} MHz apart - not a gap the conclusion can rest on"
    return; }
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

  # BOTH ENDPOINTS ARE STILL RUNNING. Without this, a station that started,
  # printed its banner and then died produces exactly the evidence this cell
  # reads as success: no association, and a lost ping. The falsifier would
  # report 4/4 having proved nothing - and it is the one cell the docs tell
  # an operator to trust. Found by review; ping_cell has had this since the
  # harness was written and cell_airgap did not inherit it.
  if ! sta_alive || ! ap_alive; then
    bad "airgap: an endpoint exited before the ping - the absence of a link proves nothing about the air"
    stop_both; return
  fi
  ok "airgap: the station found nothing to join on ch$CH in 25s, with both endpoints still running"

  ping -c 4 -W 2 -I "$STATAP" "$APIP" >"$OUT/airgap.ping" 2>&1
  if ! sta_alive || ! ap_alive; then
    bad "airgap: an endpoint exited DURING the ping - the loss proves nothing about the air"
    stop_both; return
  fi
  if grep -q " 100% packet loss" "$OUT/airgap.ping"; then
    ok "airgap: the ping is lost, all four - so the 0% loss in the cells above is the RF link and not this host"
  else
    bad "airgap: $(grep -oE '[0-9.]+% packet loss' "$OUT/airgap.ping" | head -1) with the radios on different channels - the data plane is NOT going over the air, and every other cell here is void"
  fi

  # AND THE RECEIVER WAS AWAKE WHILE IT FOUND NOTHING. A deaf radio produces
  # the same two results above as a working radio on the wrong channel.
  # Neighbour beacons are the cheapest available proof that the receiver
  # works at all, and this bench's ch6 carries about forty a second - none of
  # them ours. Read after the run, because the station prints its ledger on
  # exit. A quiet channel needs BEACONS_MIN lowered DELIBERATELY, which is
  # the point: it is the operator's call, not a silent pass.
  stop_both
  local heard
  heard=$(sed -n 's/.*beacons observed=\([0-9]*\).*/\1/p' "$OUT/sta.log" | tail -1)
  if [ "${heard:-0}" -ge "$BEACONS_MIN" ] 2>/dev/null; then
    ok "airgap: the station's receiver was working throughout - $heard beacons heard on ch$CH, none of them ours"
  else
    bad "airgap: the station heard only ${heard:-0} beacons on ch$CH (want >= $BEACONS_MIN) - a deaf receiver gives this cell its result for the wrong reason"
  fi
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
  case "$BENCH_PAYLOAD$BENCH_PPS$BENCH_SECS" in
    ''|*[!0-9]*) bad "bench: BENCH_PAYLOAD, BENCH_PPS and BENCH_SECS must all be integers"; return ;;
  esac
  # BENCH_PPS=0 divides by zero in the interval below and yields `inf`, which
  # ping accepts as "as fast as possible" - the flood this cell exists to
  # avoid.
  [ "$BENCH_PPS" -gt 0 ] && [ "$BENCH_SECS" -gt 0 ] || {
    bad "bench: BENCH_PPS and BENCH_SECS must be positive"; return; }
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
  # FRAMES *AND* NANOSECONDS. Counting frames says the cipher ran; it does
  # not say anything was TIMED, and a profile with tx_frames>0 and tx_ns=0
  # prints 0 ns/frame and passes. That is the exact bug the original bench
  # cell shipped with, one layer down. Found by review.
  local ok_counts=1 w
  for w in "$sprof" "$aprof"; do
    local t r tn rn
    t=$(printf '%s' "$w" | sed -n 's/.*"tx_frames":\([0-9][0-9]*\).*/\1/p')
    r=$(printf '%s' "$w" | sed -n 's/.*"rx_frames":\([0-9][0-9]*\).*/\1/p')
    tn=$(printf '%s' "$w" | sed -n 's/.*"tx_ns":\([0-9][0-9]*\).*/\1/p')
    rn=$(printf '%s' "$w" | sed -n 's/.*"rx_ns":\([0-9][0-9]*\).*/\1/p')
    [ "${t:-0}" -gt 0 ] 2>/dev/null && [ "${r:-0}" -gt 0 ] 2>/dev/null &&
      [ "${tn:-0}" -gt 0 ] 2>/dev/null && [ "${rn:-0}" -gt 0 ] 2>/dev/null ||
      ok_counts=0
  done
  if [ "$ok_counts" = 1 ]; then
    ok "bench: the cipher was timed at BOTH ends (frames counted AND nanoseconds accumulated)"
  else
    bad "bench: a profile counted zero frames or zero nanoseconds - the ns/frame figures below are meaningless"
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

  # FOUR EXACT IDENTITIES, two per end. Not bounds, and not sums over
  # counters that overlap - the first version of this cell added the queue
  # and device losses on top of `encrypted`, which already contains them
  # because the station counts a frame as encrypted BEFORE handing it to the
  # queue. It balanced only while both of those terms were zero, which on
  # this bench they were. Two reviewers derived the same arithmetic
  # independently; tests/sta_client_selftest.inc's test_the_books_close now
  # pins both identities headlessly, with every confounding term made
  # non-zero on purpose.
  #
  #   the host path:  from host == framed + dropped down
  #   the send path:  queued    == aired + queue dropped + send failed
  local s_from s_down s_enc s_plain s_q s_aired s_qdrop s_sfail
  s_from=$(led 'from host'); s_down=$(led 'dropped down')
  s_enc=$(sed -n 's/.*tx: encrypted=\([0-9]*\).*/\1/p' "$OUT/sta.log" | tail -1)
  s_plain=$(sed -n 's/.*, plaintext=\([0-9]*\).*/\1/p' "$OUT/sta.log" | tail -1)
  s_q=$(led 'queued'); s_aired=$(led 'aired')
  s_qdrop=$(led 'queue dropped'); s_sfail=$(led 'send failed')
  if [ "${s_from:-0}" -gt 0 ] 2>/dev/null &&
     [ $(( ${s_enc:-0} + ${s_plain:-0} + ${s_down:-0} )) = "${s_from:-0}" ] &&
     [ "${s_q:-0}" -gt 0 ] 2>/dev/null &&
     [ $(( ${s_aired:-0} + ${s_qdrop:-0} + ${s_sfail:-0} )) = "${s_q:-0}" ]; then
    ok "flood: the station's books close - $s_from from its host = $s_enc encrypted + $s_plain plaintext + $s_down refused; $s_q queued = $s_aired aired + $s_qdrop queue-dropped + $s_sfail send-failed"
  else
    bad "flood: the station's books do NOT close - host: $s_from vs $s_enc+$s_plain+$s_down; queue: $s_q vs $s_aired+$s_qdrop+$s_sfail. Frames are being lost at no counter."
  fi

  # THE AP'S BOOKS, the same two identities from the other end. `framed` is
  # its own counter rather than `frames sent` because the AP airs management
  # frames its host never sent - beacons aside, auth, assoc, the four-way and
  # the group flood all go through the same queue - so comparing the host's
  # traffic against everything aired is a bound loose enough to hide exactly
  # the loss this cell exists to catch.
  local a_from a_framed a_down a_q a_sent a_qdrop a_sfail
  a_from=$(apled 'from host'); a_framed=$(apled 'framed')
  a_down=$(apled 'dropped down'); a_q=$(apled 'queued')
  a_sent=$(apled 'frames sent'); a_qdrop=$(apled 'queue dropped')
  a_sfail=$(apled 'send failed')
  if [ "${a_from:-0}" -gt 0 ] 2>/dev/null &&
     [ $(( ${a_framed:-0} + ${a_down:-0} )) = "${a_from:-0}" ] &&
     [ "${a_q:-0}" -gt 0 ] 2>/dev/null &&
     [ $(( ${a_sent:-0} + ${a_qdrop:-0} + ${a_sfail:-0} )) = "${a_q:-0}" ]; then
    ok "flood: the AP's books close - $a_from from its host = $a_framed framed + $a_down refused; $a_q queued = $a_sent aired + $a_qdrop queue-dropped + $a_sfail send-failed"
  else
    bad "flood: the AP's books do NOT close - host: $a_from vs $a_framed+$a_down; queue: $a_q vs $a_sent+$a_qdrop+$a_sfail. Frames are being lost at no counter."
  fi

  # AND THE SEND PATH WAS ACTUALLY EXERCISED. Both identities above are
  # satisfied by a station that refused every single frame as "not connected"
  # - from host == dropped down, everything else zero - which is not nothing,
  # but it is not what the message above invites a reader to conclude.
  local f_assoc f_rc
  f_assoc=$(led 'associations'); f_rc=$(led 'reconnects')
  if [ "${s_enc:-0}" -gt 0 ] 2>/dev/null && [ "${a_framed:-0}" -gt 0 ] 2>/dev/null; then
    ok "flood: both ends framed traffic (station encrypted $s_enc, AP framed $a_framed; associations=$f_assoc reconnects=$f_rc)"
  else
    bad "flood: nothing was framed (station encrypted=$s_enc, AP framed=$a_framed) - the books balance over an empty link"
  fi

  say "  offered $tx, delivered $rx ($loss) = $(awk -v r="$rx" -v s="$BENCH_SECS" 'BEGIN{printf "%.0f", r/s}') round trips/s."
  say "  THIS IS A CEILING, NOT A THROUGHPUT: a flood ping is round-trip bound,"
  say "  and nothing in this tree measures the throughput of a station link."
}

# --- cell: throughput ------------------------------------------------------
#
# THE FIRST THING IN THIS TREE THAT MEASURES THROUGHPUT. Every station-link
# figure before this one came from a flood ping, and a flood ping is
# round-trip bound: it offers the next request only when the previous reply
# arrives. `reply_pps` is a LATENCY figure wearing a throughput costume, and
# it cannot tell a link that refuses to carry more from a link nobody asked
# more of.
#
# A LADDER OF OFFERED RATES, not a blast. The first version of this cell
# offered as fast as the socket would take it and measured 5.6 Gbit/s
# OFFERED against 5.0 Mbit/s delivered - at which point what is being
# measured is the kernel dropping on a TAP queue, and `lost` is dominated by
# frames the radio never saw. The ladder finds the knee instead: the highest
# offered rate the link carries with the loss still under THRU_LOSS_PCT.
#
# WHAT THE NUMBER IS: UDP payload goodput, one direction at a time. The
# 802.11, LLC/SNAP, CCMP, IP and UDP headers are all excluded, so it is
# strictly below what a radiotap capture would show - the honest direction to
# err in. Both directions are measured separately because an AP's downlink
# and a station's uplink are different code paths with different bugs.
#
# WHAT IT IS NOT: a PHY rate, and not a tuned link. Both ends air every frame
# at a FIXED rate (TX_RATE) with no rate control, and unless ARQ=1 there is
# no link-layer retransmission in either direction.
thru_step() {   # $1 = direction (up|down), $2 = offered kbit -> echoes the recv JSON
  local dir="$1" kbit="$2"
  if [ "$dir" = up ]; then
    ip netns exec "$NS" "$OUT/udp_blast" recv "$APIP" 5201 "$THRU_SECS" \
        >"$OUT/thru.$dir.$kbit.recv" 2>&1 &
    local rpid=$!
    sleep 1
    "$OUT/udp_blast" send "$APIP" 5201 "$THRU_PAYLOAD" "$kbit" "$THRU_SECS" \
        >"$OUT/thru.$dir.$kbit.send" 2>&1
    wait $rpid 2>/dev/null
  else
    "$OUT/udp_blast" recv "$STAIP" 5201 "$THRU_SECS" \
        >"$OUT/thru.$dir.$kbit.recv" 2>&1 &
    local rpid=$!
    sleep 1
    ip netns exec "$NS" "$OUT/udp_blast" send "$STAIP" 5201 "$THRU_PAYLOAD" \
        "$kbit" "$THRU_SECS" >"$OUT/thru.$dir.$kbit.send" 2>&1
    wait $rpid 2>/dev/null
  fi
  grep -h udp_blast.recv "$OUT/thru.$dir.$kbit.recv" 2>/dev/null | tail -1
}

# The knee: walk the ladder, print every rung, and return the best delivered
# rate whose loss is still under the threshold. Printing every rung matters -
# a single headline number hides whether the link degrades gracefully or
# falls off a cliff, and those need different fixes.
thru_ladder() {   # $1 = direction -> echoes "best_mbps best_kbit"
  local dir="$1" kbit json mbps loss best=0 best_at=0
  for kbit in $(printf '%s' "$THRU_LADDER" | tr ',' ' '); do
    json=$(thru_step "$dir" "$kbit")
    [ -n "$json" ] || { say "    ${dir} @${kbit}kbit: nothing received"; continue; }
    mbps=$(printf '%s' "$json" | sed -n 's/.*"goodput_mbps":\([0-9.]*\).*/\1/p')
    loss=$(printf '%s' "$json" | sed -n 's/.*"loss_pct":\([0-9.]*\).*/\1/p')
    say "    ${dir} offered ${kbit} kbit/s -> delivered ${mbps:-0} Mbit/s, loss ${loss:-?}%"
    printf '%s\n' "$json" | sed "s/udp_blast.recv/d2d.throughput.$dir/" >>"$OUT/thru.rows"
    if awk -v l="${loss:-100}" -v t="$THRU_LOSS_PCT" 'BEGIN{exit !(l<=t)}'; then
      if awk -v m="${mbps:-0}" -v b="$best" 'BEGIN{exit !(m>b)}'; then
        best="$mbps"; best_at="$kbit"
      fi
    fi
  done
  # THROUGH A FILE, not stdout. The per-rung lines above are stdout too, so
  # a caller reading this function's output with $( ) gets the whole table
  # and then parses the first line as the answer - which is exactly what the
  # first version of this cell did, and it reported "UP up Mbit/s".
  printf '%s %s\n' "$best" "$best_at" >"$OUT/thru.$dir.best"
}

cell_throughput() {
  local freq
  freq=$(chan_freq "$CH") || { bad "throughput: channel '$CH' is not one this harness can map"; return; }
  case "$THRU_SECS$THRU_PAYLOAD" in
    ''|*[!0-9]*) bad "throughput: THRU_SECS and THRU_PAYLOAD must be integers"; return ;;
  esac
  local steps; steps=$(printf '%s' "$THRU_LADDER" | tr ',' ' ' | wc -w)
  say "== throughput: UDP goodput, ${THRU_PAYLOAD}B, ${steps}-rung ladder x ${THRU_SECS}s each way, ch$CH ($freq MHz), rate $TX_RATE, arq=$ARQ =="

  build_both || { bad "throughput: build"; return; }
  ns_up      || { bad "throughput: could not create netns $NS"; return; }
  rm -f "$OUT/thru.rows"
  local run_secs=$(( SECS + (steps * (THRU_SECS + 2) * 2) + 30 ))
  ap_up "$CH" $((run_secs + 30)) || { bad "throughput: the AP did not come up"; stop_both; return; }
  sta_up "$CH" "$run_secs" || { bad "throughput: sta_client did not start"; stop_both; return; }
  sta_tap_up || { stop_both; return; }
  wait_for "4-WAY HANDSHAKE COMPLETE" "$OUT/ap.log" 40 || {
    bad "throughput: the four-way did not complete"; stop_both; return; }
  ping -c 1 -W 3 -I "$STATAP" "$APIP" >/dev/null 2>&1          # warm both
  ip netns exec "$NS" ping -c 1 -W 3 -I "$APTAP" "$STAIP" >/dev/null 2>&1

  local up_best=skipped up_at=- dn_best=skipped dn_at=-
  if [ "$THRU_DIR" = both ] || [ "$THRU_DIR" = up ]; then
    say "  uplink (station -> AP):"
    thru_ladder up
    read -r up_best up_at <"$OUT/thru.up.best"
  fi
  if [ "$THRU_DIR" = both ] || [ "$THRU_DIR" = down ]; then
    say "  downlink (AP -> station):"
    thru_ladder down
    read -r dn_best dn_at <"$OUT/thru.down.best"
  fi

  if ! sta_alive || ! ap_alive; then
    bad "throughput: an endpoint exited during the measurement (raise SECS, currently $SECS)"
    stop_both; return
  fi
  ok "throughput: both endpoints survived the whole ladder, both directions"
  stop_both

  # A LADDER THAT NEVER DELIVERED ANYTHING IS NOT A MEASUREMENT. The bench
  # cell in this file shipped with exactly that hole one layer down - it
  # printed 0 ns/frame over three thousand round trips and passed.
  # A DIRECTION THAT WAS NOT RUN IS NOT A DIRECTION THAT PASSED. `skipped` is
  # not a number, so awk reads it as 0 and the check fails - which is the
  # right way round: a diagnostic arm reports its rungs and does not get to
  # claim the cell's acceptance.
  if [ "$THRU_DIR" != both ]; then
    ok "throughput: $THRU_DIR only (diagnostic arm) - UP ${up_best}, DOWN ${dn_best}; see the rungs above"
    say "  THIS IS NOT THE ACCEPTANCE MEASUREMENT: one direction was skipped."
    return
  fi
  if awk -v u="${up_best:-0}" -v d="${dn_best:-0}" 'BEGIN{exit !(u>0 && d>0)}'; then
    ok "throughput: ch$CH $TX_RATE arq=$ARQ - UP ${up_best} Mbit/s (offered ${up_at} kbit/s), DOWN ${dn_best} Mbit/s (offered ${dn_at} kbit/s), both at <= ${THRU_LOSS_PCT}% loss"
  else
    bad "throughput: a direction never carried a rung under ${THRU_LOSS_PCT}% loss (up=${up_best} down=${dn_best}) - see the rungs above"
  fi
  say "  goodput is UDP PAYLOAD, one direction at a time, at a FIXED $TX_RATE"
  say "  with arq=$ARQ. It is not a PHY rate and not a tuned link."
}

# --- cell: which end starves the beacon ------------------------------------
#
# THE QUESTION THIS ANSWERS. The station drops its association under load and
# re-joins; `kBeaconLossMs` is 1024 ms, so tripping it means missing forty
# consecutive beacons at 25 TU. Two explanations fit: the AP stops SENDING
# them, or the station stops HEARING them. They need opposite fixes.
#
# A third radio in monitor mode is the obvious way to tell, and it was tried:
# it said the AP's beacon rate falls from 36.0/s idle to 1.7/s under a
# downlink flood. That run had a passing control immediately before it, on
# the same interface, at exactly the 25 TU rate - which a deaf witness cannot
# produce - so it is evidence. But the same witness then went deaf twice on
# later runs and reported zero against an AP the acceptance cell had just
# certified, and a measurement whose apparatus fails that way needs a
# corroborating method that does not use it.
#
# THIS IS THAT METHOD, and it needs no third radio. Run three phases inside
# ONE association - idle, uplink load, downlink load - and watch the
# station's own beacon counter across them:
#
#   - Under UPLINK load the station's receiver is just as busy (802.11 is
#     half duplex and it is transmitting hard) but the AP's transmit path is
#     idle.
#   - Under DOWNLINK load the AP's transmit path is saturated and the
#     station's receiver is no busier than before.
#
# So if beacon reception survives the uplink phase and collapses in the
# downlink phase, the cause is the AP's transmitter, not the station's
# receiver. Same association, same channel, same everything else - the load's
# DIRECTION is the only variable.
tick_field() {   # $1 = field -> its latest value in the station's tick stream
  sed -n "s/.*\"$1\":\([0-9][0-9]*\).*/\1/p" "$OUT/sta.log" | tail -1
}

cell_beacons() {
  local freq
  freq=$(chan_freq "$CH") || { bad "beacons: channel '$CH' is not one this harness can map"; return; }
  say "== beacons: which end starves them, ch$CH ($freq MHz), rate $TX_RATE, ${BCN_TU}TU =="

  build_both || { bad "beacons: build"; return; }
  ns_up      || { bad "beacons: could not create netns $NS"; return; }
  local phase=${BEACON_PHASE_SECS:-12}
  local run_secs=$(( SECS + phase * 4 + 40 ))
  ap_up "$CH" $((run_secs + 30)) || { bad "beacons: the AP did not come up"; stop_both; return; }
  sta_up "$CH" "$run_secs" DEVOURER_STA_TICK_MS=1000 || {
    bad "beacons: sta_client did not start"; stop_both; return; }
  sta_tap_up || { stop_both; return; }
  wait_for "4-WAY HANDSHAKE COMPLETE" "$OUT/ap.log" 40 || {
    bad "beacons: the four-way did not complete"; stop_both; return; }
  ping -c 1 -W 3 -I "$STATAP" "$APIP" >/dev/null 2>&1
  ip netns exec "$NS" ping -c 1 -W 3 -I "$APTAP" "$STAIP" >/dev/null 2>&1
  wait_for '"ev":"sta.tick"' "$OUT/sta.log" 10 || {
    bad "beacons: the station emitted no tick stream - DEVOURER_STA_TICK_MS ignored?"
    stop_both; return; }

  local b0 b1 r0 r1 idle_rate up_rate down_rate up_rc down_rc
  local o0 o1 idle_ours up_ours down_ours
  # -- phase 1: idle. This is the CONTROL: if the station does not see close
  # to the configured beacon rate with nothing happening, nothing below means
  # anything, and the cell says so instead of reporting a ratio.
  b0=$(tick_field beacons); o0=$(tick_field beacons_ours)
  sleep "$phase"
  b1=$(tick_field beacons); o1=$(tick_field beacons_ours)
  idle_rate=$(awk -v a="${b0:-0}" -v b="${b1:-0}" -v s="$phase" 'BEGIN{printf "%.1f", (b-a)/s}')
  idle_ours=$(awk -v a="${o0:-0}" -v b="${o1:-0}" -v s="$phase" 'BEGIN{printf "%.1f", (b-a)/s}')
  # 25 TU is 25.6 ms, so 39.1/s; the floor is deliberately loose because a
  # beacon lost to a neighbour's transmission is not the effect under test.
  local want_rate; want_rate=$(awk -v tu="$BCN_TU" 'BEGIN{printf "%.1f", 1000.0/(tu*1.024)}')
  if awk -v r="$idle_rate" -v w="$want_rate" 'BEGIN{exit !(r > w*0.5)}'; then
    ok "beacons: control - idle the station sees ${idle_rate}/s against ${want_rate}/s aired (${BCN_TU} TU)"
  else
    bad "beacons: control FAILED - idle the station sees only ${idle_rate}/s against ${want_rate}/s aired; the phases below would be ratios of noise"
    stop_both; return
  fi

  # -- phase 2: uplink load. The station transmits hard; the AP does not.
  r0=$(tick_field reconnects); b0=$(tick_field beacons); o0=$(tick_field beacons_ours)
  "$OUT/udp_blast" send "$APIP" 5201 "$THRU_PAYLOAD" "$BEACON_KBIT" "$phase" \
      >"$OUT/beacons.up.send" 2>&1
  b1=$(tick_field beacons); o1=$(tick_field beacons_ours); r1=$(tick_field reconnects)
  up_rate=$(awk -v a="${b0:-0}" -v b="${b1:-0}" -v s="$phase" 'BEGIN{printf "%.1f", (b-a)/s}')
  up_ours=$(awk -v a="${o0:-0}" -v b="${o1:-0}" -v s="$phase" 'BEGIN{printf "%.1f", (b-a)/s}')
  up_rc=$(( ${r1:-0} - ${r0:-0} ))

  # -- phase 3: downlink load. The AP transmits hard; the station does not.
  r0=$(tick_field reconnects); b0=$(tick_field beacons); o0=$(tick_field beacons_ours)
  ip netns exec "$NS" "$OUT/udp_blast" send "$STAIP" 5201 "$THRU_PAYLOAD" \
      "$BEACON_KBIT" "$phase" >"$OUT/beacons.down.send" 2>&1
  b1=$(tick_field beacons); o1=$(tick_field beacons_ours); r1=$(tick_field reconnects)
  down_rate=$(awk -v a="${b0:-0}" -v b="${b1:-0}" -v s="$phase" 'BEGIN{printf "%.1f", (b-a)/s}')
  down_ours=$(awk -v a="${o0:-0}" -v b="${o1:-0}" -v s="$phase" 'BEGIN{printf "%.1f", (b-a)/s}')
  down_rc=$(( ${r1:-0} - ${r0:-0} ))

  # -- phase 4: IDLE AGAIN. Did the beacon survive the load? It lives in the
  # chip's reserved page and the hardware airs it at every TBTT from one
  # download. It used NOT to survive: the TX data page ring ran on into the
  # reserved region, a sustained load overwrote the beacon page, and the next
  # TBTT read a data frame as a beacon descriptor and latched a TX-DMA fault
  # (measured - the page read 5a 5a 5a..., the injected fill byte). Fixed by
  # enabling the whole MAC (halmac MAC_TRX_ENABLE = 0xFF) before the LLT init
  # in HalmacJaguar3MacInit, so the hardware wraps the ring at the reserved
  # boundary. This phase is the regression check for it: a beacon that does
  # not come back means the page was lost.
  sleep 2
  b0=$(tick_field beacons); o0=$(tick_field beacons_ours)
  sleep "$phase"
  b1=$(tick_field beacons); o1=$(tick_field beacons_ours)
  local after_ours
  after_ours=$(awk -v a="${o0:-0}" -v b="${o1:-0}" -v s="$phase" 'BEGIN{printf "%.1f", (b-a)/s}')

  if ! sta_alive || ! ap_alive; then
    bad "beacons: an endpoint exited during the phases (raise SECS, currently $SECS)"
    stop_both; return
  fi
  stop_both

  say "  phase      all BSSes    ours only    reconnects"
  say "  idle       ${idle_rate}/s        ${idle_ours}/s"
  say "  uplink     ${up_rate}/s        ${up_ours}/s         ${up_rc}"
  say "  downlink   ${down_rate}/s        ${down_ours}/s         ${down_rc}"
  say "  idle again ${after_ours}/s (ours)"

  # THE RULE, and it is the half that should hold: the station's receiver is
  # every bit as busy transmitting as it is receiving, so if beacon reception
  # were a property of how loaded the STATION is, the uplink phase would
  # collapse too. Asserting it here means the downlink figure printed above
  # is a statement about the AP rather than about load in general.
  if awk -v u="$up_rate" -v i="$idle_rate" -v f="$BEACON_FLOOR_PCT" \
         'BEGIN{exit !(i>0 && 100*u/i >= f)}'; then
    ok "beacons: under UPLINK load the station still hears $(awk -v u="$up_rate" -v i="$idle_rate" 'BEGIN{printf "%.0f", 100*u/i}')% of idle - a busy station is not the cause"
  else
    bad "beacons: uplink load alone costs the station its beacons (${up_rate}/s vs ${idle_rate}/s idle) - the receiver IS the bottleneck and the downlink figure proves nothing about the AP"
  fi

  # STARVED OR CLOBBERED. This is the one that says which fix to write.
  if awk -v a="$after_ours" -v i="$idle_ours" 'BEGIN{exit !(i>0 && a >= i*0.8)}'; then
    ok "beacons: the beacon comes straight back when the load stops (${after_ours}/s vs ${idle_ours}/s idle) - the beacon page survived the load"
  else
    bad "beacons: the beacon does NOT recover when the load stops (${after_ours}/s vs ${idle_ours}/s idle) - the beacon page was lost; check MAC_TRX_ENABLE in HalmacJaguar3MacInit (0xFF before the LLT init), and run ap_wpa2 with DEVOURER_AP_INJECT=4000 DEVOURER_AP_PKTBUF=1: LLT[1937] must read 0 at the end of run"
  fi

  # THE DIRECTION DISCRIMINATOR, and it is the ratio of two ratios rather than either
  # one. Under downlink load the station's receiver is busy, so SOME beacon
  # loss is expected from every BSS on the channel. What separates "the AP
  # stopped sending" from "the receiver stopped hearing" is whether OUR
  # beacons fall further than everyone else's, measured in the same seconds
  # on the same radio.
  say "  VERDICT: $(awk -v do_="$down_ours" -v io="$idle_ours" \
                        -v da="$down_rate" -v ia="$idle_rate" 'BEGIN{
        if (io<=0 || ia<=0) { print "no control"; exit }
        ours   = 100*do_/io
        theirs_n = da - do_; theirs_d = ia - io
        if (theirs_d <= 0) {
          printf "ours fell to %.0f%% of idle; no neighbour beacons to compare against", ours
          exit
        }
        theirs = 100*theirs_n/theirs_d
        printf "ours %.0f%% of idle, neighbours %.0f%% of idle", ours, theirs
        if (ours >= 80)
          printf "  -> the beacon SURVIVES downlink load"
        else if (ours < theirs*0.6)
          printf "  -> THE AP STOPPED SENDING (ours fell much further)"
        else
          printf "  -> both fell alike: the STATION RECEIVER is the bottleneck, not the AP" }')"
}

# --- cell: the soak --------------------------------------------------------
#
# EVERY FIGURE IN THIS BRANCH CAME FROM A RUN OF 45 TO 160 SECONDS. That is
# long enough to prove a link comes up and short enough to miss everything
# that only happens on the hundredth rekey, the ten-thousandth frame, or the
# first time a counter wraps. This is the cell that runs long.
#
# BIDIRECTIONAL. Each chunk loads the uplink (SOAK_KBIT) and the downlink
# (SOAK_DOWN_KBIT) at once. It was uplink-only until the AP's transmit path
# stopped wedging under downlink load (REG_CR at the LLT init - see
# docs/jaguar3-tx-ring.md); SOAK_DOWN_KBIT=0 still gives that shape. A
# two-packet ping per chunk stays as an independent liveness probe.
#
# WHAT IT GRADES, and none of it is "it did not crash":
#   - ONE association for the whole run. A soak that silently re-associates
#     every ten minutes is a soak that proves nothing about the link.
#   - The books close at both ends AFTER the long run, not just after a
#     short one - a counter that drifts by one frame per thousand is
#     invisible at 40 seconds and obvious at 30 minutes.
#   - The AP's books too, now that it carries real host traffic.
#   - Throughput in the LAST quarter against the FIRST, per direction. Degradation over
#     time is the thing a soak exists to find and a single end-of-run
#     average hides it completely.
#   - Resident memory at both ends, first sample against last.
cell_soak() {
  local freq
  freq=$(chan_freq "$CH") || { bad "soak: channel '$CH' is not one this harness can map"; return; }
  case "$SOAK_MINUTES$SOAK_KBIT$SOAK_DOWN_KBIT$SOAK_CHUNK_S" in
    ''|*[!0-9]*) bad "soak: SOAK_MINUTES, SOAK_KBIT, SOAK_DOWN_KBIT and SOAK_CHUNK_S must be integers"; return ;;
  esac
  [ "$SOAK_MINUTES" -gt 0 ] || { bad "soak: SOAK_MINUTES must be positive"; return; }
  local total=$(( SOAK_MINUTES * 60 ))
  local chunks=$(( total / SOAK_CHUNK_S ))
  [ "$chunks" -ge 4 ] || { bad "soak: need at least 4 chunks (SOAK_MINUTES*60 / SOAK_CHUNK_S = $chunks)"; return; }
  say "== soak: ${SOAK_MINUTES} min, ${SOAK_KBIT} kbit/s up + ${SOAK_DOWN_KBIT} kbit/s down in ${chunks} x ${SOAK_CHUNK_S}s chunks, ch$CH ($freq MHz), rate $TX_RATE =="

  build_both || { bad "soak: build"; return; }
  ns_up      || { bad "soak: could not create netns $NS"; return; }
  local run_secs=$(( SECS + total + 120 ))
  ap_up "$CH" $((run_secs + 60)) || { bad "soak: the AP did not come up"; stop_both; return; }
  sta_up "$CH" "$run_secs" DEVOURER_STA_TICK_MS=1000 || {
    bad "soak: sta_client did not start"; stop_both; return; }
  sta_tap_up || { stop_both; return; }
  wait_for "4-WAY HANDSHAKE COMPLETE" "$OUT/ap.log" 40 || {
    bad "soak: the four-way did not complete"; stop_both; return; }
  ping -c 1 -W 3 -I "$STATAP" "$APIP" >/dev/null 2>&1
  ip netns exec "$NS" ping -c 1 -W 3 -I "$APTAP" "$STAIP" >/dev/null 2>&1

  local sta_pid ap_pid rss0_s rss0_a
  sta_pid=$(pgrep -P "$STA_PID_RUN" -x sta_client | head -1)
  ap_pid=$(ip netns exec "$NS" pgrep -x ap_wpa2 2>/dev/null | head -1)
  [ -z "$ap_pid" ] && ap_pid=$(pgrep -x ap_wpa2 | head -1)
  rss0_s=$(awk '/VmRSS/{print $2}' "/proc/${sta_pid:-0}/status" 2>/dev/null)
  rss0_a=$(awk '/VmRSS/{print $2}' "/proc/${ap_pid:-0}/status" 2>/dev/null)

  rm -f "$OUT/soak.chunks"
  local c down_ok=0 down_try=0
  for c in $(seq 1 "$chunks"); do
    ip netns exec "$NS" "$OUT/udp_blast" recv "$APIP" 5201 "$SOAK_CHUNK_S" \
        >"$OUT/soak.c$c.recv" 2>&1 &
    local rp=$! dp="" dsp=""
    if [ "$SOAK_DOWN_KBIT" -gt 0 ]; then
      "$OUT/udp_blast" recv "$STAIP" 5202 "$SOAK_CHUNK_S" \
          >"$OUT/soak.c$c.drecv" 2>&1 &
      dp=$!
    fi
    sleep 1
    if [ -n "$dp" ]; then
      ip netns exec "$NS" "$OUT/udp_blast" send "$STAIP" 5202 "$THRU_PAYLOAD" \
          "$SOAK_DOWN_KBIT" "$SOAK_CHUNK_S" >"$OUT/soak.c$c.dsend" 2>&1 &
      dsp=$!
    fi
    "$OUT/udp_blast" send "$APIP" 5201 "$THRU_PAYLOAD" "$SOAK_KBIT" \
        "$SOAK_CHUNK_S" >"$OUT/soak.c$c.send" 2>&1
    wait $rp $dp $dsp 2>/dev/null
    local mbps loss dmbps=0 dloss=-1
    mbps=$(sed -n 's/.*"goodput_mbps":\([0-9.]*\).*/\1/p' "$OUT/soak.c$c.recv" | tail -1)
    loss=$(sed -n 's/.*"loss_pct":\(-\?[0-9.]*\).*/\1/p' "$OUT/soak.c$c.recv" | tail -1)
    if [ -n "$dp" ]; then
      dmbps=$(sed -n 's/.*"goodput_mbps":\([0-9.]*\).*/\1/p' "$OUT/soak.c$c.drecv" | tail -1)
      dloss=$(sed -n 's/.*"loss_pct":\(-\?[0-9.]*\).*/\1/p' "$OUT/soak.c$c.drecv" | tail -1)
    fi
    printf '%s %s %s %s %s\n' "$c" "${mbps:-0}" "${loss:--1}" \
        "${dmbps:-0}" "${dloss:--1}" >>"$OUT/soak.chunks"
    # An independent liveness probe of the downlink, two pings per chunk,
    # separate from the load.
    down_try=$((down_try+1))
    ip netns exec "$NS" ping -c 2 -W 2 -I "$APTAP" "$STAIP" >/dev/null 2>&1 &&
      down_ok=$((down_ok+1))
    sta_alive && ap_alive || break
    say "    chunk $c/$chunks: up ${mbps:-0} Mbit/s loss ${loss:--}%, down ${dmbps:-0} Mbit/s loss ${dloss:--}%  (ping $down_ok/$down_try)"
  done

  if ! sta_alive || ! ap_alive; then
    bad "soak: an endpoint exited during the run - it did not survive ${SOAK_MINUTES} minutes"
    stop_both; return
  fi
  ok "soak: both endpoints ran for ${SOAK_MINUTES} minutes"

  local rss1_s rss1_a
  rss1_s=$(awk '/VmRSS/{print $2}' "/proc/${sta_pid:-0}/status" 2>/dev/null)
  rss1_a=$(awk '/VmRSS/{print $2}' "/proc/${ap_pid:-0}/status" 2>/dev/null)
  stop_both

  # -- one association throughout
  local assoc rc mic
  assoc=$(led 'associations'); rc=$(led 'reconnects'); mic=$(led 'MIC failures')
  if [ "${assoc:-0}" = 1 ] && [ "${rc:-1}" = 0 ]; then
    ok "soak: ONE association for the whole run (reconnects=0)"
  else
    bad "soak: the link did not hold - associations=$assoc reconnects=$rc over ${SOAK_MINUTES} min"
  fi

  # -- the books still close after a long run
  local s_from s_down s_enc s_plain s_q s_aired s_qdrop s_sfail
  s_from=$(led 'from host'); s_down=$(led 'dropped down')
  s_enc=$(sed -n 's/.*tx: encrypted=\([0-9]*\).*/\1/p' "$OUT/sta.log" | tail -1)
  s_plain=$(sed -n 's/.*, plaintext=\([0-9]*\).*/\1/p' "$OUT/sta.log" | tail -1)
  s_q=$(led 'queued'); s_aired=$(led 'aired')
  s_qdrop=$(led 'queue dropped'); s_sfail=$(led 'send failed')
  if [ $(( ${s_enc:-0} + ${s_plain:-0} + ${s_down:-0} )) = "${s_from:-0}" ] &&
     [ $(( ${s_aired:-0} + ${s_qdrop:-0} + ${s_sfail:-0} )) = "${s_q:-0}" ]; then
    ok "soak: the station's books still close after ${SOAK_MINUTES} min ($s_from host frames, $s_q queued)"
  else
    bad "soak: the station's books DRIFTED over the run - host: $s_from vs $s_enc+$s_plain+$s_down; queue: $s_q vs $s_aired+$s_qdrop+$s_sfail"
  fi

  # -- the AP's books and the per-direction trend (shared with soak-grade)
  soak_grade_tail

  # -- the downlink stayed alive, and memory did not run away
  say "  downlink reachable in $down_ok of $down_try chunks"
  say "  RSS station ${rss0_s:-?} -> ${rss1_s:-?} kB, AP ${rss0_a:-?} -> ${rss1_a:-?} kB"
  say "  MIC failures over the whole run: ${mic:-?}"
  awk '{printf "    chunk %s: up %s Mbit/s loss %s%%, down %s Mbit/s loss %s%%\n", $1, $2, $3, $4, $5}' "$OUT/soak.chunks"
}

# Expected check counts, so the advertised score is machine-enforced rather
# than counted by eye - a future edit that drops an ok()/bad() pair would
# otherwise run one check fewer, exit 0, and still be read as "N/N".
case "$CELLS" in
  wpa2)    cell_link wpa2 "$CH";           want=8 ;;
  fiveghz) cell_link fiveghz "$CH5";       want=8 ;;
  airgap)  cell_airgap;                    want=5 ;;
  bench)   cell_bench;                     want=3 ;;
  flood)   cell_flood;                     want=3 ;;
  thru)    cell_throughput;                want=2 ;;
  beacons) cell_beacons;                   want=3 ;;
  soak)    cell_soak;                      want=5 ;;
  all)     cell_link wpa2 "$CH"; cleanup
           cell_link fiveghz "$CH5"; cleanup
           cell_airgap;                    want=21 ;;
  *)       echo "usage: $0 [wpa2|fiveghz|airgap|bench|flood|thru|beacons|soak|all] | soak-grade DIR"; exit 2 ;;
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
