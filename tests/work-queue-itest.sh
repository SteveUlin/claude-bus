#!/usr/bin/env bash
# work-queue-itest.sh — end-to-end integration test for M2 (the work-queue as a
# Policy DispatchActor; docs/work-queue-dispatch.md). Spins up an ISOLATED broker
# in a throwaway $STATE (never the live one) and proves the central-push model:
#   - a task enqueued to a work-queue topic is ASSIGNED to an idle agent's inbox
#     by the broker's DispatchActor (no per-agent fetch, no hooks);
#   - the queue is DRAINED (the head consumed — the claim, performed by the loop
#     on the actor's consume_from intent, reusing the fetch primitive).
# Touches nothing live; cleans up on exit.

set -uo pipefail

# Binary from THIS tree (workspace-relative), so the workspace build is tested.
BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/work-queue-itest.XXXXXX)"
AUDIT_DUMP="$CLAUDE_BUS_STATE/broker.out"
fail=0

note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }
# RPC activity drives broker ticks (the loop is RPC-driven). Poke a few times.
tick() { for _ in 1 2 3 4; do "$BUS" state >/dev/null 2>&1; sleep 0.25; done; }

# Fake zellij: resolve the worker's pane (paneId() shells out to list-panes);
# empty screen / no-op on everything else.
FAKE="$CLAUDE_BUS_STATE/fakebin"; mkdir -p "$FAKE"
cat > "$FAKE/zellij" <<'EOF'
#!/usr/bin/env bash
# paneId() parses PRETTY-printed list-panes (key: value with a space, one field
# per line) — match that format exactly or the title needle misses.
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

# Seed worker1 as idle at a ready prompt (last event Stop → turn=Ready), so the
# DispatchActor sees an eligible agent on the first tick after a task arrives.
NOW_TS="$(date -u +%FT%T.%3NZ)"
cat > "$CLAUDE_BUS_STATE/events.jsonl" <<EOF
{"ts":"$NOW_TS","session":"t","agent":"worker1","pane":"1","event":"Stop","payload":{}}
EOF

"$BUS" broker run >"$AUDIT_DUMP" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1; done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$AUDIT_DUMP"; exit 1
fi
echo "isolated broker up (pid $BPID)"

note "create the work-queue topic + enqueue one task"
"$BUS" topic create work-queue --kind work-queue >/dev/null 2>&1
ck "$?" 0 "work-queue topic created"
"$BUS" msg enqueue work-queue "WQ-TASK-ALPHA" >/dev/null 2>&1
ck "$?" 0 "task enqueued"

note "drive ticks — the DispatchActor assigns the head to the idle worker"
tick

WQLOG="$CLAUDE_BUS_STATE/topics/work-queue.log"
INBOX="$CLAUDE_BUS_STATE/topics/inbox-worker1.log"

# The task reached the worker's inbox (central push — no fetch by the agent).
if grep -aq "WQ-TASK-ALPHA" "$INBOX" 2>/dev/null; then assigned=yes; else assigned=no; fi
ck "$assigned" "yes" "task assigned to inbox-worker1 (central push)"

# The queue head was consumed (the claim): peek from the "" cursor is now empty.
peek_out="$("$BUS" msg peek work-queue 2>/dev/null)"
if echo "$peek_out" | grep -aq "WQ-TASK-ALPHA"; then drained=no; else drained=yes; fi
ck "$drained" "yes" "work-queue head consumed (queue drained)"

note "no task left stranded in the queue"
# A second tick must not re-assign (the head was claimed → nothing to do).
before="$(grep -ac "WQ-TASK-ALPHA" "$INBOX" 2>/dev/null || echo 0)"
tick
after="$(grep -ac "WQ-TASK-ALPHA" "$INBOX" 2>/dev/null || echo 0)"
ck "$before" "$after" "no duplicate re-assignment after the claim"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mWORK-QUEUE DISPATCH CHECK PASSED\033[0m"
else
  echo -e "\033[1mWORK-QUEUE DISPATCH CHECK FAILED\033[0m"
  echo "broker.out tail:"; tail -20 "$AUDIT_DUMP" 2>/dev/null
fi
exit "$fail"
