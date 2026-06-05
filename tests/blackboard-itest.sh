#!/usr/bin/env bash
# blackboard-itest.sh — end-to-end integration test for the blackboard fan-out
# Policy actor (the second coordination pattern; docs/blackboard-notify.md).
# Isolated broker in a throwaway $STATE. Proves:
#   - a new blackboard post fans out a notify to every live agent EXCEPT the
#     author (1→N broadcast, the complement to the work-queue's 1→1 claim);
#   - it does not re-notify on a later tick (the loop-owned _dispatch cursor
#     advanced — consume-on-read).
# Touches nothing live; cleans up on exit.

set -uo pipefail

BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/blackboard-itest.XXXXXX)"
fail=0

note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }
tick() { for _ in 1 2 3 4; do "$BUS" state >/dev/null 2>&1; sleep 0.25; done; }

# Fake zellij: two idle worker panes (pretty-printed — paneId needs the spaces).
FAKE="$CLAUDE_BUS_STATE/fakebin"; mkdir -p "$FAKE"
cat > "$FAKE/zellij" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *list-panes*)
    cat <<'JSON'
{
  "tabs": [
    {
      "panes": [
        {
          "id": 1,
          "title": "worker1",
          "is_plugin": false
        },
        {
          "id": 2,
          "title": "worker2",
          "is_plugin": false
        }
      ]
    }
  ]
}
JSON
    ;;
  *) : ;;
esac
exit 0
EOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

# Both workers idle at a ready prompt.
NOW_TS="$(date -u +%FT%T.%3NZ)"
cat > "$CLAUDE_BUS_STATE/events.jsonl" <<EOF
{"ts":"$NOW_TS","session":"t","agent":"worker1","pane":"1","event":"Stop","payload":{}}
{"ts":"$NOW_TS","session":"t","agent":"worker2","pane":"2","event":"Stop","payload":{}}
EOF

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1; done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID)"

note "create the blackboard topic + worker1 posts a note"
"$BUS" topic create notes --kind blackboard >/dev/null 2>&1
ck "$?" 0 "blackboard topic created"
CLAUDE_BUS_AGENT_ID=worker1 "$BUS" msg enqueue notes "BOARD-NOTE-ALPHA" >/dev/null 2>&1
ck "$?" 0 "worker1 posted to the board"

note "drive ticks — the BlackboardActor fans out to all but the author"
tick

IN1="$CLAUDE_BUS_STATE/topics/inbox-worker1.log"
IN2="$CLAUDE_BUS_STATE/topics/inbox-worker2.log"

# worker2 (not the author) is notified; worker1 (author) is NOT.
if grep -aq "BOARD-NOTE-ALPHA" "$IN2" 2>/dev/null; then n2=yes; else n2=no; fi
ck "$n2" "yes" "worker2 (non-author) got the blackboard-notify"
if grep -aq "BOARD-NOTE-ALPHA" "$IN1" 2>/dev/null; then n1=yes; else n1=no; fi
ck "$n1" "no" "worker1 (author) was NOT notified"
# The notify carries the blackboard-notify protocol tag.
if grep -aq "blackboard-notify" "$IN2" 2>/dev/null; then tag=yes; else tag=no; fi
ck "$tag" "yes" "notify carries the blackboard-notify protocol"

note "no re-notify on a later tick (the _dispatch cursor advanced)"
before="$(grep -ac "BOARD-NOTE-ALPHA" "$IN2" 2>/dev/null || echo 0)"
tick
after="$(grep -ac "BOARD-NOTE-ALPHA" "$IN2" 2>/dev/null || echo 0)"
ck "$before" "$after" "no duplicate notify after the cursor advanced"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mBLACKBOARD FAN-OUT CHECK PASSED\033[0m"
else
  echo -e "\033[1mBLACKBOARD FAN-OUT CHECK FAILED\033[0m"
  echo "broker.out tail:"; tail -20 "$CLAUDE_BUS_STATE/broker.out" 2>/dev/null
fi
exit "$fail"
