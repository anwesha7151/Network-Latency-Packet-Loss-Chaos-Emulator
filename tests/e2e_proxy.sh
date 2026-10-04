#!/usr/bin/env bash
# End-to-end test, no root needed: probe -> chaos proxy -> echo server, on localhost.
# Verifies that the *running system* really produces the requested latency and loss.
set -u
cd "$(dirname "$0")/.."
BIN=./chaos; EP=19100; PP=19101; FAIL=0
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
val() { echo "$1" | grep "\"$2\"" | sed 's/[^0-9.]*\([0-9.]*\).*/\1/'; }
check() { # name actual lo hi
  if awk -v a="$2" -v lo="$3" -v hi="$4" 'BEGIN{exit !(a>=lo && a<=hi)}'; then echo "  PASS $1 = $2 (expected $3..$4)"
  else echo "  FAIL $1 = $2 (expected $3..$4)"; FAIL=1; fi; }

$BIN echo --port $EP 2>/dev/null & PIDS+=($!)

echo "[1] latency: 40 ms each way => RTT ~80 ms"
$BIN proxy --listen $PP --target 127.0.0.1:$EP --set delay=40 2>/dev/null & P=$!; sleep 0.3
R=$($BIN probe --target 127.0.0.1:$PP --count 60 --interval 15 --json)
kill $P; wait $P 2>/dev/null
check "rtt_avg_ms" "$(val "$R" rtt_avg_ms)" 79 86
check "loss_pct"   "$(val "$R" loss_pct)"   0 0

echo "[2] loss: 20% each way => end-to-end loss ~36% (1-0.8^2)"
$BIN proxy --listen $PP --target 127.0.0.1:$EP --set loss=20 --seed 7 2>/dev/null & P=$!; sleep 0.3
R=$($BIN probe --target 127.0.0.1:$PP --count 500 --interval 2 --json)
kill $P; wait $P 2>/dev/null
check "loss_pct" "$(val "$R" loss_pct)" 30 42

echo "[3] duplication is detected by the probe"
$BIN proxy --listen $PP --target 127.0.0.1:$EP --set dup=20 --seed 3 2>/dev/null & P=$!; sleep 0.3
R=$($BIN probe --target 127.0.0.1:$PP --count 300 --interval 2 --json)
kill $P; wait $P 2>/dev/null
check "dups" "$(val "$R" dups)" 30 150

echo "[4] scenario: clean at first, then 100 ms each way (step lands mid-run)"
printf 'at 0 none\nat 1.8 delay=100\n' > /tmp/chaos_e2e.scn
$BIN proxy --listen $PP --target 127.0.0.1:$EP --scenario /tmp/chaos_e2e.scn 2>/dev/null & P=$!; sleep 0.3
OUT=$($BIN probe --target 127.0.0.1:$PP --count 100 --interval 25 --timeline)
kill $P; wait $P 2>/dev/null
echo "$OUT" | sed 's/^/    /'
rtt_at() { echo "$OUT" | sed -n "s/.*t=$1s .*avg RTT=\([0-9.]*\).*/\1/p"; }
T0=$(rtt_at 0); T2=$(rtt_at 2)
check "rtt before step (t=0s)" "${T0:-999}" 0 10
check "rtt after step  (t=2s)" "${T2:-0}" 195 215

[ $FAIL -eq 0 ] && echo "E2E: all passed" || { echo "E2E: FAILURES"; exit 1; }
