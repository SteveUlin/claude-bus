#!/usr/bin/env bash
# inbox-drain.sh EVENT_NAME
#
# OFF-TTY delivery hook (roadmap 2.1 / transport §5.1). Wired as a
# UserPromptSubmit + SessionStart hook, this drains the agent's own
# inbox via the broker's `drain` RPC and emits the pending mail as
# `additionalContext` — replacing the broker's TTY write. The pane stays
# the human's.
#
# Thin wrapper: `bus msg drain` does the work (off-TTY policy check,
# presence gate, epoch/TTL fence, cursor advance, additionalContext
# framing) and prints either an additionalContext JSON object or
# nothing. Safe to run every turn.
#
# Off-TTY is the FLEET DEFAULT, so this drains for every agent — EXCEPT
# the durable TTY opt-out set (comms + $CLAUDE_BUS_TTY_AGENTS), for whom
# the `drain` RPC returns empty (they stay on the broker's TTY push).
# The policy lives in the broker (tty_policy.h), authoritative — the
# hook stays dumb so the opt-out can never drift between the two.

set -uo pipefail

EVENT="${1:-UserPromptSubmit}"
NAME="${CLAUDE_BUS_AGENT_ID:-}"
[ -n "$NAME" ] || exit 0  # not a fleet agent

# Do NOT drain on a post-/compact SessionStart. /compact's additionalContext
# does not surface to the model (the summarised context never replays it),
# so draining here would CONSUME the mail (advance the cursor) into a void —
# the exact post-compaction strand that hung the fleet. Leave the mail
# QUEUED instead: the broker's doorbell wakes the now-idle agent (its wake
# gate admits process=Compacting) and the resulting [bus-wake]
# UserPromptSubmit drains it through a live turn that DOES surface it. This
# is a delivery-timing skip, distinct from the TTY opt-out policy (still the
# broker's, via the drain RPC); the hook gates it only because the source
# rides in on stdin, which the broker never sees.
if [ "$EVENT" = "SessionStart" ]; then
  case "$(cat)" in
    *'"source":"compact"'*) exit 0 ;;
  esac
fi

BUS="$(dirname "$0")/../../bin/bus"
[ -x "$BUS" ] || BUS="bus"

exec "$BUS" msg drain "$NAME" "$EVENT"
