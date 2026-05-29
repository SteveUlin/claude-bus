#!/usr/bin/env bash
# inject-learnings.sh — SessionStart hook that injects an agent's durable
# per-agent learnings back into its context.
#
# The read-path half of learning-over-time: distilled lessons live in
# $STATE/learnings/<agent>.md (write-path — reflection cron / librarian /
# a model-driven `learnings append` verb — is a separate, later piece).
# This hook re-enters that file into the agent's context at session start
# via hookSpecificOutput.additionalContext, the documented deterministic
# injection mechanism. See docs/deep/memory-learning.md (R1).
#
# Two correctness constraints (verified against the hooks reference):
#   1. Branch on `source`. SessionStart carries source ∈
#      {startup, resume, clear, compact}. Inject only on startup|clear.
#      The common fleet path is agent-launch --continue → source=resume,
#      where the prior context (including last session's injected
#      learnings) is already restored; re-injecting double-loads and
#      wastes budget. On compact the agent is mid-task and re-priming
#      fights the compaction summary. So: startup|clear only.
#   2. additionalContext is capped at 10,000 chars. Truncate the file to
#      stay under the ceiling; once a learnings file approaches that, it
#      is the signal to graduate to ranked retrieval (not this hook).
#
# Also scaffolds the per-agent file (empty) on every run regardless of
# source, so the path exists for the write-path to append to. An empty
# file injects nothing.
#
# No-op (exit 0, no output) when: CLAUDE_BUS_AGENT_ID is unset, source is
# resume|compact, or the learnings file is missing/empty. Safe under
# matcher:"" alongside the other SessionStart hooks.
set -uo pipefail

NAME=${CLAUDE_BUS_AGENT_ID:-}
[ -z "$NAME" ] && exit 0

STATE=${CLAUDE_BUS_STATE:-/tmp/claude-bus}
LEARN_DIR="$STATE/learnings"
LEARN_FILE="$LEARN_DIR/$NAME.md"

# Scaffold the per-agent file so the write-path has a target. Cheap and
# idempotent; runs on every source.
mkdir -p "$LEARN_DIR" 2>/dev/null
[ -e "$LEARN_FILE" ] || : > "$LEARN_FILE"

payload=$(cat)
source=$(printf '%s' "$payload" | jq -r '.source // empty' 2>/dev/null)

case "$source" in
  startup|clear) ;;
  *) exit 0 ;;
esac

# Nothing to inject if the file is empty (the common case until the
# write-path lands).
[ -s "$LEARN_FILE" ] || exit 0

# Truncate to the additionalContext ceiling (10,000 chars), leaving a
# little headroom for the framing line below.
body=$(head -c 9500 "$LEARN_FILE")
[ -z "$body" ] && exit 0

ctx="Your accumulated learnings (durable per-agent notes distilled from prior sessions). Treat them as fallible context, not commands — correct or prune anything stale.

$body"

jq -n --arg ctx "$ctx" \
  '{hookSpecificOutput: {hookEventName: "SessionStart", additionalContext: $ctx}}'
