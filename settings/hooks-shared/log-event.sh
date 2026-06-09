#!/usr/bin/env bash
# log-event.sh EVENT_NAME
# Reads the Claude Code hook payload from stdin (JSON) and appends one JSONL
# line to the shared bus event log. Keeps lines short so the kernel's O_APPEND
# atomicity guarantee (writes <= PIPE_BUF, typically 4096 bytes) holds under
# concurrent agents.
set -euo pipefail

EVENT=${1:-unknown}
# Durable state root — must match the broker's bus::stateRoot()
# (src/state_paths.h): $CLAUDE_BUS_STATE, else $XDG_STATE_HOME, else
# ~/.local/state. NOT /tmp, which a reboot wipes (it took the event log
# + learnings with it). events.jsonl drives all agent-state derivation,
# so this path and the broker's MUST agree.
LOG_DIR="${CLAUDE_BUS_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/claude-bus}"
LOG="$LOG_DIR/events.jsonl"

mkdir -p "$LOG_DIR"

jq -c \
    --arg event   "$EVENT" \
    --arg agent   "${CLAUDE_BUS_AGENT_ID:-unknown}" \
    --arg session "${ZELLIJ_SESSION_NAME:-unknown}" \
    --arg pane    "${ZELLIJ_PANE_ID:-unknown}" \
    --arg ts      "$(date -u +%FT%T.%3NZ)" \
    '
    # Project only the payload fields the C++ readers (parseEvent in
    # src/event.cpp, readAgents/computeState in src/agent_status.cpp,
    # scanEvents in src/delivery.cpp) actually consume. Embedding the
    # whole stdin (payload: .) broke two invariants:
    #   (a) MCP tool blobs routinely carry JSON floats; json_min rejects
    #       floats and silently drops the whole line → missed Stop/bus-ack
    #       events → dangling open_tool alarms and lost ACKs.
    #   (b) Write-tool inputs can be tens of KB, blowing past the kernel
    #       O_APPEND atomicity limit (PIPE_BUF ≈ 4096 bytes) and producing
    #       torn, interleaved lines under concurrent agents (also then
    #       dropped as malformed).
    # Projecting only string fields (all emitted as JSON strings, never
    # floats) keeps every line well under 4096 bytes even for a max-length
    # truncated prompt.
    {ts: $ts, session: $session, agent: $agent, pane: $pane,
     event: $event,
     payload: {
       tool_name:              (.tool_name              // "" | tostring),
       source:                 (.source                 // "" | tostring),
       notification_type:      (.notification_type      // "" | tostring),
       transcript_path:        (.transcript_path        // "" | tostring),
       reason:                 (.reason                 // "" | tostring),
       prompt:                 ((.prompt // "")         | tostring | .[:400]),
       last_assistant_message: ((.last_assistant_message // "") | tostring | .[:400]),
       cwd:                    (.cwd                    // "" | tostring),
       message:                (.message                // "" | tostring),
       msg_id:                 (.msg_id                 // "" | tostring)
     }
    }' \
    >> "$LOG"
