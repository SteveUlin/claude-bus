#!/usr/bin/env bash
# Stop hook: drain CLAUDE_BUS_AGENT_ID's mailbox; if anything was queued,
# emit it to stderr and exit 2. Per Claude Code's hook semantics, Stop +
# exit 2 + stderr = "don't go idle; here's feedback to process." The agent
# treats the mailbox content as a new turn and keeps working until the box
# drains empty — then the next Stop returns 0 and the agent finally idles.
#
# This is the self-pulling pattern: once the agent has done ANY turn,
# every subsequent Stop checks the mailbox for free. Senders only need a
# wake nudge for cold-start (recipient at first prompt, never processed).
set -euo pipefail

MAILBOX=/home/sulin/claude-bus/bin/mailbox
agent="${CLAUDE_BUS_AGENT_ID:-}"
if [ -z "$agent" ]; then
    exit 0
fi

out=$("$MAILBOX" drain "$agent" 2>/dev/null || true)
if [ -z "$out" ]; then
    exit 0
fi

printf '## bus mailbox (auto-drained on Stop; keep processing)\n\n%s\n' "$out" >&2
exit 2
