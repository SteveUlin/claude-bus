#!/usr/bin/env bash

# This script provides functions to start and stop an isolated broker daemon.
# It requires the $BUS environment variable to be set to the bus binary.

start_isolated_broker() {
    export CLAUDE_BUS_STATE=$(mktemp -d -t bus-test.XXXXXX)
    export BROKER_STATE=$CLAUDE_BUS_STATE
    
    "$BUS" broker run > "$CLAUDE_BUS_STATE/broker.log" 2>&1 &
    export BROKER_PID=$!

    # Wait for the Unix Socket to become available
    sleep 0.5
    if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
        echo "FAIL: Isolated broker failed to create socket!"
        cat "$CLAUDE_BUS_STATE/broker.log"
        kill "$BROKER_PID" 2>/dev/null || true
        rm -rf "$CLAUDE_BUS_STATE"
        exit 1
    fi
}

stop_isolated_broker() {
    if [ -n "${BROKER_PID:-}" ]; then
        kill "$BROKER_PID" 2>/dev/null || true
        wait "$BROKER_PID" 2>/dev/null || true
        unset BROKER_PID
    fi
    if [ -n "${CLAUDE_BUS_STATE:-}" ] && [ -d "$CLAUDE_BUS_STATE" ]; then
        rm -rf "$CLAUDE_BUS_STATE"
    fi
}
