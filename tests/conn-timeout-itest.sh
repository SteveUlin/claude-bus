#!/usr/bin/env bash
# conn-timeout-itest.sh — broker-hardening CRIT #2 regression (elodin).
#
# The listen fd is non-blocking (#13), but a plain ::accept yields a BLOCKING
# conn. A client that connects, writes a partial line (no '\n'), and never
# closes parks the SINGLE intake thread in readLine's ::read forever → the
# broker goes RPC-deaf (delivery still ticks, masking it). Fix: SO_RCVTIMEO +
# SO_SNDTIMEO on the accepted conn — a stalled client is cut after the timeout,
# intake resumes.
#
# Regression: hold a silent connection open, then assert a SECOND normal RPC
# still completes (within ~CONN_TIMEOUT_MS + margin). Pre-fix the second RPC
# hangs forever (timeout kills it → fail); post-fix it returns once the silent
# conn is cut.
#
# Silent client = nc -U feeding a partial line then blocking on a FIFO that is
# never written (so nc holds the conn open without sending a newline). python3
# isn't on the dev PATH; nc + timeout are.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/conn-timeout-itest.XXXXXX)"
export CLAUDE_BUS_CONN_TIMEOUT_MS=1000   # cut a stalled client after 1 s
SOCK="$CLAUDE_BUS_STATE/broker.sock"
HOLD="$CLAUDE_BUS_STATE/hold.fifo"
mkfifo "$HOLD"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
cleanup() {
  kill "$BPID" 2>/dev/null
  [ -n "${NCPID:-}" ] && kill "$NCPID" 2>/dev/null
  # Unblock the FIFO cat if still parked.
  : > "$HOLD" 2>/dev/null
  wait "$BPID" 2>/dev/null
  rm -rf "$CLAUDE_BUS_STATE"
}
trap cleanup EXIT
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
if [ ! -S "$SOCK" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID), conn-timeout=${CLAUDE_BUS_CONN_TIMEOUT_MS}ms"

echo "1. baseline: a normal RPC works"
ck "$(timeout 5 "$BUS" broker info >/dev/null 2>&1 && echo ok)" "ok" \
   "baseline 'bus broker info' responds"

echo "2. open a SILENT client: partial line, no newline, held open"
# printf the partial request, then block reading the never-written FIFO so nc
# keeps the connection open with no terminating '\n'.
( printf '%s' '{"op":"info"'; cat "$HOLD" ) | nc -U "$SOCK" &
NCPID=$!
sleep 0.4   # let intake accept it and park in readLine on the silent conn

echo "3. ASSERT the broker is STILL responsive (intake not parked forever)"
t0=$(date +%s%3N)
rc=0
timeout 5 "$BUS" broker info >/dev/null 2>&1 || rc=$?
t1=$(date +%s%3N)
ck "$rc" "0" "second RPC completes despite the silent client (rc=$rc; 124=timed-out=PRE-FIX wedge)"
echo "    (second RPC latency: $((t1 - t0)) ms; expected ~<= conn-timeout + margin)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mCONN-TIMEOUT ITEST PASSED (silent client cannot wedge intake)\033[0m"
else
  echo -e "\033[1mCONN-TIMEOUT ITEST FAILED — intake parked by a silent client (pre-fix)\033[0m"
  echo "broker.out:"; cat "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
