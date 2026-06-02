#!/usr/bin/env bash
# gc-reap-itest.sh — broker-GC orphan-topic reaping (elodin).
#
# `bus broker gc` drops agent-inbox / tui-commands topics whose agent is gone
# AND whose log is drained. Two gates, both must hold to reap:
#   - not-a-live-agent: no agents/<X>.json, not in dynamic-peers, not reserved
#     infra (human/ops).
#   - drained: no in-flight + default cursor at log EOF (zero unread).
# Targeted mode (`gc agent=NAME`, used by `bus despawn`) is explicit operator
# intent: it bypasses the live-agent gate but STILL honors the drained gate
# (never drop unread mail).
#
# Cases:
#   A1 orphan + drained (empty topic)         -> REAPED   (sweep)
#   A2 live agent (agents/alice.json) + drained -> skipped live-agent
#   A3 orphan + undrained (one unread record) -> skipped undrained
#   A4 reserved infra (inbox-ops)             -> skipped live-agent
#   A5 live-but-despawned + drained, targeted -> REAPED   (live-gate bypassed)
#
# Harness notes (project_broker_itest_gotchas + fake-zellij): the fake zellij
# exposes NO panes, so the delivery loop defers every push (records stay
# unread) — drained-state is deterministic without driving ack timing. A high
# ack timeout keeps escalation from advancing a cursor mid-test. Assert on
# `bus topic list` membership + the gc reaped/skipped report, never a racy read.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/gc-reap-itest.XXXXXX)"
export CLAUDE_BUS_ACK_TIMEOUT_MS=999999   # no escalation during the test

FAKE="$CLAUDE_BUS_STATE/fakebin"
mkdir -p "$FAKE" "$CLAUDE_BUS_STATE/agents"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# Fake zellij: no panes, no tabs — every delivery defers, every tab query
# misses (so despawn skips the close and proceeds to the reap RPC).
cat > "$FAKE/zellij" <<'FAKEEOF'
#!/usr/bin/env bash
case "$*" in
  *"list-panes"*) echo '{}' ;;   # no panes -> paneId fails -> push defers
  *) : ;;
esac
exit 0
FAKEEOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID)"

echo "seed topics + liveness"
# A1 orphan + drained (empty)
"$BUS" topic create inbox-ghost --kind agent-inbox >/dev/null
# A2 live agent + drained (empty)
echo '{"name":"alice"}' > "$CLAUDE_BUS_STATE/agents/alice.json"
"$BUS" topic create inbox-alice --kind agent-inbox >/dev/null
# A3 orphan + undrained (one unread record; no pane -> stays unread)
"$BUS" msg mail zombie "still here" >/dev/null
# A4 reserved infra: ensure inbox-ops exists + empty
"$BUS" topic create inbox-ops --kind agent-inbox >/dev/null 2>&1 || true
# A5 live-but-despawned + drained
echo '{"name":"live2"}' > "$CLAUDE_BUS_STATE/agents/live2.json"
"$BUS" topic create inbox-live2 --kind agent-inbox >/dev/null

has() { "$BUS" topic list 2>/dev/null | grep -q "^$1 " && echo yes || echo no; }

echo "1. SWEEP: bus broker gc"
GC_OUT="$("$BUS" broker gc 2>&1)"
echo "$GC_OUT" | sed 's/^/    /'

echo "2. ASSERT reap + skip outcomes (sweep)"
ck "$(echo "$GC_OUT" | grep -c '^reaped  inbox-ghost$')" "1" "orphan+drained inbox-ghost REAPED"
ck "$(has inbox-ghost)" "no" "inbox-ghost gone from registry"
ck "$(echo "$GC_OUT" | grep -c 'skipped inbox-alice .*live-agent')" "1" "live agent inbox-alice skipped (live-agent)"
ck "$(has inbox-alice)" "yes" "inbox-alice retained"
ck "$(echo "$GC_OUT" | grep -c 'skipped inbox-zombie .*undrained')" "1" "orphan+undrained inbox-zombie skipped (undrained)"
ck "$(has inbox-zombie)" "yes" "inbox-zombie retained (unread mail preserved)"
ck "$(echo "$GC_OUT" | grep -c 'skipped inbox-ops .*live-agent')" "1" "reserved inbox-ops skipped (live-agent)"
ck "$(has inbox-ops)" "yes" "inbox-ops retained"

echo "3. TARGETED: bus despawn live2 (bypasses live-gate, honors drained)"
DESP_OUT="$("$BUS" despawn live2 2>&1)"
echo "    $DESP_OUT"
ck "$(echo "$DESP_OUT" | grep -c '1 broker topic(s) reaped')" "1" "despawn reaped inbox-live2 (live-gate bypassed)"
ck "$(has inbox-live2)" "no" "inbox-live2 gone after targeted despawn"
# alice was NOT despawned -> still protected
ck "$(has inbox-alice)" "yes" "non-despawned live agent inbox-alice still retained"

echo "4. ASSERT cursor dir + log file removed for a reaped topic"
ck "$([ -e "$CLAUDE_BUS_STATE/cursors/inbox-ghost" ] && echo present || echo gone)" "gone" "inbox-ghost cursor dir removed"
ck "$([ -e "$CLAUDE_BUS_STATE/topics/inbox-ghost.log" ] && echo present || echo gone)" "gone" "inbox-ghost log file removed"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mGC-REAP ITEST PASSED (orphan reap; live + undrained + reserved protected; targeted bypass)\033[0m"
else
  echo -e "\033[1mGC-REAP ITEST FAILED\033[0m"
  echo "broker.out:"; cat "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
