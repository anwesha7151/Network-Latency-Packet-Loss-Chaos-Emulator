#!/usr/bin/env bash
# Regression tests (no root needed) for bugs found in review:
#   1. mistyped / unknown options were silently ignored (a chaos run with NO impairment)
#   2. numeric options were not validated (negative -> huge unsigned, 0 -> nan, 70000-byte UDP payload ...)
#   3. proxy spun at ~100% CPU after the target went down (pending ICMP error never consumed)
#   4. proxy never released per-client sockets, then silently ignored every new client
set -u
cd "$(dirname "$0")/.."
BIN=./chaos; EP=19200; PP=19201; DEAD=19209; FAIL=0
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
ok()  { echo "  PASS $1"; }
bad() { echo "  FAIL $1"; FAIL=1; }

# expect_err NAME SUBSTRING ARGS... : must exit with status 2 and mention SUBSTRING on stderr
expect_err() {
  local name=$1 want=$2 out rc; shift 2
  out=$("$BIN" "$@" 2>&1 >/dev/null); rc=$?
  if [ $rc -eq 2 ] && grep -qF -- "$want" <<<"$out"; then ok "$name"; else bad "$name (rc=$rc, stderr: $out)"; fi
}

echo "[1] unknown options / stray arguments are rejected"
expect_err "typo --profle"            "unknown option '--profle'"  sim --profle 3g
expect_err "typo suggests --profile"  "did you mean '--profile'"   sim --profle 3g
expect_err "typo --pakets"            "unknown option '--pakets'"  sim --profile 3g --pakets 10
expect_err "option of another cmd"    "unknown option '--listen'"  sim --listen 9000
expect_err "flag of another cmd"      "unknown option '--json'"    proxy --listen 19250 --target 127.0.0.1:9 --json
expect_err "stray positional"         "unexpected argument 'banana'" sim banana
expect_err "flag with a value"        "does not take a value"      sim --json=1
if "$BIN" sim --help | grep -q USAGE; then ok "sim --help prints usage"; else bad "sim --help prints usage"; fi

echo "[2] numeric options are validated"
expect_err "sim --packets -5"   "--packets must be between"  sim --packets -5
expect_err "sim --packets 0"    "--packets must be between"  sim --packets 0
expect_err "sim --packets 1.5"  "whole number"               sim --packets 1.5
expect_err "sim --pps 0"        "--pps must be between"      sim --pps 0
expect_err "sim --pps -10"      "--pps must be between"      sim --pps -10
expect_err "sim --size 0"       "--size must be between"      sim --size 0
expect_err "sim --seed -1"      "--seed must be between"     sim --seed -1
expect_err "sim --pps abc"      "expects a number"           sim --pps abc
expect_err "probe --size 70000" "--size must be between"     probe --target 127.0.0.1:9 --size 70000
expect_err "probe --size -5"    "--size must be between"     probe --target 127.0.0.1:9 --size -5
expect_err "probe --wait -5"    "--wait must be between"     probe --target 127.0.0.1:9 --wait -5
expect_err "proxy --listen 99999" "--listen must be between" proxy --listen 99999 --target 127.0.0.1:9
expect_err "target junk port"   "bad HOST:PORT"              proxy --listen 19250 --target 127.0.0.1:80abc
expect_err "netem scenario w/o --file" "--file"              netem scenario --dev eth0 --dry-run

echo "[3] proxy does not busy-loop when the target is down"
$BIN proxy --listen $PP --target 127.0.0.1:$DEAD --duration 10 2>/dev/null & P=$!; PIDS+=($P); sleep 0.3
$BIN probe --target 127.0.0.1:$PP --count 5 --interval 20 --wait 200 >/dev/null
sleep 0.5
t0=$(awk '{print $14+$15}' /proc/$P/stat); sleep 1.5; t1=$(awk '{print $14+$15}' /proc/$P/stat)
used=$((t1 - t0)); limit=$(( $(getconf CLK_TCK) * 3 / 10 ))   # 0.3 CPU-seconds in 1.5 s (a spinning proxy uses ~1.2)
if [ $used -le $limit ]; then ok "idle CPU after target-down = $used ticks (limit $limit)"; else bad "proxy spins: $used ticks in 1.5 s (limit $limit)"; fi
kill $P 2>/dev/null; wait $P 2>/dev/null

echo "[4] proxy releases / recycles client sockets (fd limit 64)"
$BIN echo --port $EP 2>/dev/null & PIDS+=($!)
one() { "$BIN" probe --target 127.0.0.1:$PP --count 1 --interval 1 --wait 300 --json | grep -c '"received": 1'; }
batch() { local n=$1 got=0 i; for ((i = 0; i < n; i++)); do got=$((got + $(one))); done; echo $got; }

( ulimit -n 64; exec $BIN proxy --listen $PP --target 127.0.0.1:$EP --idle 1 2>/dev/null ) & P=$!; PIDS+=($P); sleep 0.4
a=$(batch 35); sleep 3.2; b=$(batch 35)
[ "$a" -eq 35 ] && [ "$b" -eq 35 ] && ok "idle expiry: 35 + 35 clients served with only ~58 free fds" || bad "idle expiry: served $a/35 then $b/35"
kill $P 2>/dev/null; wait $P 2>/dev/null

( ulimit -n 64; exec $BIN proxy --listen $PP --target 127.0.0.1:$EP 2>/dev/null ) & P=$!; PIDS+=($P); sleep 0.4
c=$(batch 120)
[ "$c" -eq 120 ] && ok "fd exhaustion: 120 distinct clients served by evicting the oldest" || bad "fd exhaustion: served only $c/120"
kill $P 2>/dev/null; wait $P 2>/dev/null

[ $FAIL -eq 0 ] && echo "CLI/robustness: all passed" || { echo "CLI/robustness: FAILURES"; exit 1; }
