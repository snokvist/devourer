#!/usr/bin/env bash
# fast_bw_parity.sh — validate FastSetBandwidth (the lean same-channel
# 20<->5/10 MHz toggle) on a plugged adapter, three ways:
#   1. timing     — full SetMonitorChannel vs fast FastSetBandwidth (median us)
#   2. parity     — the BW re-clock registers come out bit-for-bit identical to
#                   the full narrowband path. The register set is per
#                   generation: J1 0x8ac, J2 0x8c4/0x8c8, J3 0x9b0/0x9b4 plus
#                   the 8822E MAC-clock half (0x24/0x55c/0x638) and its
#                   bandwidth-keyed spur state (0x818/0x1940/0x4040/0x1944/
#                   0x4044/0x1d3c/0xc0c/0xc24/0x810/0x88c). Run it on ch 153
#                   for an 8822E: 153/161/169 are the only channels where a
#                   20<->5/10 toggle crosses a spur-combo boundary, so ch36
#                   cannot see a stale-notch regression at all.
#   3. cross-RX   — with a narrowband TX partner, the RX decodes ONLY in the
#                   fast-toggled narrowband window (different clock domain from
#                   20 MHz), proving the fast switch re-clocks on-air
#
# Builds the two throwaway harnesses against the already-built static lib.
#
#   sudo tests/fast_bw_parity.sh <RX_VID> <RX_PID> [CH] [TX_PID] [NB]
# e.g. Jaguar2 8822B RX + 8812AU 10 MHz TX partner:
#   sudo tests/fast_bw_parity.sh 0x2357 0x012d 36 0x8812 10
# 8822E spur-boundary arm (the case the ch36 default cannot reach):
#   sudo tests/fast_bw_parity.sh 0x0bda 0xa81a 153 0x8812 10
set -uo pipefail
cd "$(dirname "$0")/.."

RX_VID="${1:-0x0bda}"
RX_PID="${2:?usage: $0 <RX_VID> <RX_PID> [ch] [tx_pid] [nb]}"
CH="${3:-36}"
TX_PID="${4:-0x8812}"   # narrowband TX partner (a second adapter)
NB="${5:-10}"
LIBS="$(pkg-config --cflags --libs libusb-1.0)"

build() { g++ -std=c++20 -O2 -Isrc -Iexamples/common "tests/$1.cpp" \
    examples/common/env_config.cpp build/libdevourer.a $LIBS -lpthread \
    -o "build/$1" || { echo "build $1 failed"; exit 1; }; }
build retune_bench
build fast_bw_rxcheck

cleanup() { pkill -9 -x retune_bench 2>/dev/null; pkill -9 -x fast_bw_rxcheck 2>/dev/null;
            pkill -9 -x txdemo 2>/dev/null; }
trap cleanup EXIT
cleanup; sleep 1

echo "############ 1. TIMING (full vs fast) ############"
sudo env DEVOURER_VID="$RX_VID" DEVOURER_PID="$RX_PID" DEVOURER_CHANNEL="$CH" \
    DEVOURER_LOG_LEVEL=silent build/retune_bench 15 2>/dev/null | grep -A6 "bandwidth-switch"
cleanup; sleep 1

echo
echo "############ 2. REGISTER PARITY (re-clock + spur state, full vs fast) ############"
# One grep over every generation's BW-keyed set — a chip only dumps the
# registers its own canary lists, so the union is safe to ask for everywhere.
# J1 0x8ac; J2 0x8c4/0x8c8; J3 0x9b0/0x9b4 re-clock, MAC 0x24/0x55c/0x638
# (the 8822E MAC-clock half of the narrowband switch), and the 8822E spur
# state, which is keyed on (channel, BANDWIDTH) and so has to survive a
# bandwidth toggle: 0x818/0x1940/0x4040/0x1944/0x4044/0x1d3c/0xc0c/0xc24 plus
# the NBI workaround params in 0x810/0x88c.
# The canary zero-pads to 3 or 4 hex digits depending on the generation
# (J1 "BB 0x8ac", J3 "BB 0x09b0"), so every address is matched with a leading
# 0* rather than a fixed width.
PARITY_REGS="BB 0x0*8ac |BB 0x0*8c4 |BB 0x0*8c8 |BB 0x0*9b0 |BB 0x0*9b4 "
PARITY_REGS+="|BB 0x0*810 |BB 0x0*818 |BB 0x0*88c |BB 0x0*c0c |BB 0x0*c24 "
PARITY_REGS+="|BB 0x0*1940 |BB 0x0*4040 |BB 0x0*1944 |BB 0x0*4044 |BB 0x0*1d3c "
PARITY_REGS+="|MAC 0x0*24 |MAC 0x0*55c |MAC 0x0*638 "
sudo env DEVOURER_VID="$RX_VID" DEVOURER_PID="$RX_PID" DEVOURER_CHANNEL="$CH" \
    PARITY=1 DEVOURER_DUMP_CANARY=1 DEVOURER_LOG_LEVEL=info build/retune_bench 2>&1 |
  grep -iE "PARITY_MARK|$PARITY_REGS"
echo
echo "Compare each 'PARITY_MARK full N' block against the 'fast N' block that"
echo "follows it — every listed register must match. On an 8822E at ch 153 the"
echo "spur registers are the ones that regress if the fast path stops"
echo "reprogramming the notch; at ch 36 they are constant and prove nothing."
cleanup; sleep 1

echo
echo "############ 3. CROSS-RX (fast-toggle decodes narrowband on-air) ############"
echo "TX partner $TX_PID at ${NB} MHz narrowband; RX $RX_PID toggles via fast path"
sudo env DEVOURER_PID="$TX_PID" DEVOURER_CHANNEL="$CH" DEVOURER_NB_BW="$NB" \
    DEVOURER_TX_GAP_US=0 DEVOURER_LOG_LEVEL=silent build/txdemo >/tmp/fbw-tx.log 2>&1 &
sleep 4
sudo env DEVOURER_VID="$RX_VID" DEVOURER_PID="$RX_PID" DEVOURER_CHANNEL="$CH" \
    DEVOURER_LOG_LEVEL=silent build/fast_bw_rxcheck "$NB" 2>/dev/null
