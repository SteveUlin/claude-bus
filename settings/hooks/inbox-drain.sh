#!/usr/bin/env bash
# inbox-drain.sh EVENT_NAME
#
# OFF-TTY delivery hook (roadmap 2.1 / transport §5.1). Wired as a
# UserPromptSubmit + SessionStart hook, this drains the agent's own
# inbox via the broker's `drain` RPC and emits the pending mail as
# `additionalContext` — replacing the broker's TTY write. The pane stays
# the human's.
#
# Thin wrapper: `bus msg drain` does the work (presence gate, epoch/TTL
# fence, cursor advance, additionalContext framing) and prints either an
# additionalContext JSON object or nothing. Safe to run every turn.
#
# NOT YET referenced by settings/claude-settings.json — wiring it there
# (+ creating $STATE/off-tty/<agent>) is the single change that flips an
# agent off-TTY. See docs/prototypes/off-tty-delivery/README.md.

set -uo pipefail

EVENT="${1:-UserPromptSubmit}"
NAME="${CLAUDE_BUS_AGENT_ID:-}"
[ -n "$NAME" ] || exit 0  # not a fleet agent

BUS="$(dirname "$0")/../../bin/bus"
[ -x "$BUS" ] || BUS="bus"

exec "$BUS" msg drain "$NAME" "$EVENT"
