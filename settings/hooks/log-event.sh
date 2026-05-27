#!/usr/bin/env bash
# log-event.sh EVENT_NAME
# Reads the Claude Code hook payload from stdin (JSON) and appends one JSONL
# line to the shared bus event log. Keeps lines short so the kernel's O_APPEND
# atomicity guarantee (writes <= PIPE_BUF, typically 4096 bytes) holds under
# concurrent agents.
set -euo pipefail

EVENT=${1:-unknown}
LOG_DIR=/tmp/claude-bus
LOG="$LOG_DIR/events.jsonl"

mkdir -p "$LOG_DIR"

jq -c \
    --arg event   "$EVENT" \
    --arg agent   "${CLAUDE_BUS_AGENT_ID:-unknown}" \
    --arg session "${ZELLIJ_SESSION_NAME:-unknown}" \
    --arg pane    "${ZELLIJ_PANE_ID:-unknown}" \
    --arg ts      "$(date -u +%FT%T.%3NZ)" \
    '{ts: $ts, session: $session, agent: $agent, pane: $pane, event: $event, payload: .}' \
    >> "$LOG"
