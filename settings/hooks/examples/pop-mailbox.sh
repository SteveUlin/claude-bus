#!/usr/bin/env bash
# Alternative Stop hook for "one-task-at-a-time" mode.
# Pops a SINGLE unread message and feeds it back via Stop+exit2 so the
# agent processes one task per turn — useful when each task needs a fresh
# context and shouldn't be batched with siblings.
#
# To use: in .claude/settings.json, swap drain-mailbox.sh's UserPromptSubmit
# entry for nothing (so prompts aren't auto-drained), and register THIS
# script as a Stop hook. Then `bus send <agent> "[bus-wake]"` (or the
# watcher's nudge) triggers the first turn; this hook pops one mail per
# subsequent turn until the box is empty.
set -euo pipefail

MAILBOX=/home/sulin/claude-bus/bin/mailbox
agent="${CLAUDE_BUS_AGENT_ID:-}"
if [ -z "$agent" ]; then
    exit 0
fi

out=$("$MAILBOX" pop "$agent" 2>/dev/null || true)
if [ -z "$out" ]; then
    exit 0
fi

printf '## bus mailbox: next task (one-at-a-time mode)\n\n%s\n' "$out" >&2
exit 2
