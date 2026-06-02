#!/usr/bin/env bash
# Regression: the broker must stop promptly on SIGTERM even after it has
# served a request.
#
# The Pillar-D intake loop drains accept() in an inner while(true). With a
# *blocking* listen fd, the second accept() parks forever between requests,
# and SIGTERM there returns EINTR → the loop re-accept()s and swallows the
# stop — the broker can only be killed with SIGKILL. An *idle* broker (still
# in the top pselect) stops fine, so the bug hides unless the broker has
# handled at least one connection first. This test reproduces that state:
# poke the broker with one RPC, then SIGTERM and assert it dies fast.
#
# Fix: SOCK_NONBLOCK on the listen fd (drain → EAGAIN → break → stop-
# responsive pselect) + readLine/writeAll re-checking gStopFlag on EINTR.

set -uo pipefail

BUS_ROOT=${CLAUDE_BUS_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}
BUS=${BUS:-$BUS_ROOT/bin/bus}

STATE=$(mktemp -d -t bus-shutdown.XXXXXX)
export CLAUDE_BUS_STATE=$STATE
deadline_ms=3000

cleanup() { rm -rf "$STATE"; }
trap cleanup EXIT

"$BUS" broker run > "$STATE/broker.log" 2>&1 &
bpid=$!

# Wait for the socket so the broker is past boot and listening.
for _ in $(seq 1 20); do
    [ -S "$STATE/broker.sock" ] && break
    sleep 0.1
done
if [ ! -S "$STATE/broker.sock" ]; then
    echo "FAIL  broker never created its socket"
    cat "$STATE/broker.log"
    kill -KILL "$bpid" 2>/dev/null
    exit 1
fi

# Serve one request so intake leaves the top pselect and parks in the inner
# accept() drain loop — the state that used to swallow SIGTERM.
"$BUS" inflight > /dev/null 2>&1

# SIGTERM and measure how long the broker takes to exit.
kill -TERM "$bpid"
waited=0
while [ "$waited" -lt "$deadline_ms" ]; do
    kill -0 "$bpid" 2>/dev/null || break
    sleep 0.1
    waited=$(( waited + 100 ))
done

if kill -0 "$bpid" 2>/dev/null; then
    echo "FAIL  broker still alive ${deadline_ms}ms after SIGTERM (shutdown wedge)"
    kill -KILL "$bpid" 2>/dev/null
    wait "$bpid" 2>/dev/null
    exit 1
fi
wait "$bpid" 2>/dev/null
echo "PASS  broker stopped on SIGTERM after serving a request (~${waited}ms)"
exit 0
