#!/usr/bin/env bash
# inbox-drain.sh EVENT_NAME
#
# PROTOTYPE off-TTY delivery hook (roadmap 2.1 / transport §5.1, §6.2).
# Wired as a UserPromptSubmit + SessionStart hook, this REPLACES the
# broker's TTY write: instead of the broker typing mail into the pane,
# the agent drains its own inbox at a clean turn boundary and emits the
# records as `additionalContext`. The pane stays the human's.
#
# This prototype reads a simple newline-delimited JSON inbox
# ($STATE/inbox-proto/<agent>.ndjson) that sim-broker-write.sh appends
# to. In the real integration the broker would write to the existing
# agent-inbox topic log and the hook would read it via a `bus` read
# verb (see README §"Mapping to the real broker"). The drain logic —
# cursor, presence gate, idempotency, additionalContext framing — is
# identical either way; only the record source changes.
#
# NOT wired into settings.json. Run demo.sh to exercise it in isolation.

set -uo pipefail

EVENT="${1:-UserPromptSubmit}"
NAME="${CLAUDE_BUS_AGENT_ID:-}"
STATE="${CLAUDE_BUS_STATE:-/tmp/claude-bus}"
[ -n "$NAME" ] || exit 0  # not a fleet agent — nothing to drain

INBOX="$STATE/inbox-proto/$NAME.ndjson"
CURSOR="$STATE/inbox-proto/$NAME.cursor"  # line count already drained
SEEN="$STATE/inbox-proto/$NAME.seen"      # msg_ids already emitted (dedup)
PRESENCE="$STATE/presence/$NAME"

[ -f "$INBOX" ] || exit 0

# ---- presence gate (transport §6b.4) -------------------------------
# The whole point of the [bus-attach] sentinel is "the human has the
# keyboard, don't inject." additionalContext interrupts a human-driven
# turn just as much as a TTY write, so the gate MUST move with delivery.
# Defer (cursor untouched) while the sentinel is fresh (<1h, mirrors
# hasPresenceFile's expiry).
if [ -f "$PRESENCE" ]; then
    now=$(date +%s)
    mt=$(stat -c %Y "$PRESENCE" 2>/dev/null || echo 0)
    if [ $((now - mt)) -lt 3600 ]; then
        exit 0
    fi
fi

cursor=$(cat "$CURSOR" 2>/dev/null || echo 0)
total=$(wc -l < "$INBOX" 2>/dev/null || echo 0)
[ "$total" -gt "$cursor" ] || exit 0  # nothing new past the cursor

# ---- drain past the cursor, dedup by msg_id ------------------------
touch "$SEEN"
emitted=""
line_no=0
new_cursor=$cursor
while IFS= read -r rec; do
    line_no=$((line_no + 1))
    [ "$line_no" -gt "$cursor" ] || continue
    new_cursor=$line_no
    mid=$(printf '%s' "$rec" | jq -r '.msg_id // empty' 2>/dev/null)
    [ -n "$mid" ] || continue
    # Idempotency (transport §6.5): a retrying/replaying transport is
    # at-least-once; refuse a msg_id already emitted so active replay or
    # a double-write can't inject the same record twice.
    if grep -qxF -- "$mid" "$SEEN"; then
        continue
    fi
    echo "$mid" >>"$SEEN"
    sender=$(printf '%s' "$rec" | jq -r '.sender // "unknown"' 2>/dev/null)
    body=$(printf '%s' "$rec" | jq -r '.body // ""' 2>/dev/null)
    emitted+="## bus mail from ${sender}"$'\n'"${body}"$'\n\n'
done <"$INBOX"

printf '%s' "$new_cursor" >"$CURSOR"
[ -n "$emitted" ] || exit 0

# ---- emit as additionalContext -------------------------------------
# hookSpecificOutput.additionalContext is "added more discretely" — it
# does not render as a visible hook block in the transcript, which is
# what we want for a clean injected user turn. hookEventName must match
# the firing event (UserPromptSubmit or SessionStart).
jq -cn --arg ev "$EVENT" --arg ctx "$emitted" \
    '{hookSpecificOutput: {hookEventName: $ev, additionalContext: $ctx}}'
exit 0
