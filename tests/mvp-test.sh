#!/usr/bin/env bash
# Minimal isolated test framework and MVP unit test
set -euo pipefail

BUS_ROOT=${CLAUDE_BUS_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}
BUS=${BUS:-$BUS_ROOT/bin/bus}

echo "=== Setting up Isolated Broker Environment ==="
# 1. Create an isolated state directory
export CLAUDE_BUS_STATE=$(mktemp -d -t bus-test.XXXXXX)
echo "State dir: $CLAUDE_BUS_STATE"

# 2. Start broker in background
"$BUS" broker run > "$CLAUDE_BUS_STATE/broker.log" 2>&1 &
BROKER_PID=$!

# Wait for broker to initialize the UNIX socket
sleep 0.5
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
    echo "FAIL: Broker failed to create socket!"
    cat "$CLAUDE_BUS_STATE/broker.log"
    kill "$BROKER_PID" || true
    rm -rf "$CLAUDE_BUS_STATE"
    exit 1
fi

echo "Broker running with PID $BROKER_PID"

# 3. Setup cleanup trap to kill broker and wipe state
cleanup() {
    echo "=== Cleaning up ==="
    kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
    rm -rf "$CLAUDE_BUS_STATE"
}
trap cleanup EXIT

# ---------------------------------------------------------
# MVP Test Cases
# ---------------------------------------------------------

echo "=== Running MVP Unit Test ==="

echo "[Test] Create topic..."
"$BUS" topic create test-topic --kind work-queue

echo "[Test] Enqueue message..."
"$BUS" msg enqueue test-topic "hello isolated world"

echo "[Test] Fetch message..."
MSG=$("$BUS" msg fetch test-topic)

if echo "$MSG" | grep -q "hello isolated world"; then
    echo "PASS: Message successfully routed through isolated broker!"
else
    echo "FAIL: Message not found or broker failed."
    echo "Response was: $MSG"
    exit 1
fi
