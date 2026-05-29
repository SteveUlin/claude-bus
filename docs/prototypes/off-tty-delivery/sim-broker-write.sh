#!/usr/bin/env bash
# sim-broker-write.sh AGENT BODY [SENDER]
#
# Stands in for the broker's delivery side: append one record to the
# prototype inbox. In the real integration this is `TopicLog::append`
# to inbox-<agent>; here it's one NDJSON line so the prototype is
# self-contained and shell-only. The broker NEVER touches the TTY —
# that's the whole point.

set -euo pipefail

NAME="${1:?usage: sim-broker-write.sh AGENT BODY [SENDER]}"
BODY="${2:?usage: sim-broker-write.sh AGENT BODY [SENDER]}"
SENDER="${3:-broker}"
STATE="${CLAUDE_BUS_STATE:-/tmp/claude-bus}"

mkdir -p "$STATE/inbox-proto"
mid="$(date +%s%3N)-${SENDER}-${RANDOM}"
jq -cn --arg id "$mid" --arg s "$SENDER" --arg b "$BODY" \
    '{msg_id: $id, sender: $s, body: $b}' \
    >>"$STATE/inbox-proto/$NAME.ndjson"
echo "wrote $mid -> $STATE/inbox-proto/$NAME.ndjson"
