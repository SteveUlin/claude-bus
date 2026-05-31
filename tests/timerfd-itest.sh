#!/usr/bin/env bash
# timerfd-itest.sh — D8 Part B quiet-fleet proof. The whole claim is that a
# wedged tool escalates within budget on the TIMERFD ALONE, with ZERO RPC
# traffic — not riding a viewer-poll/RPC tick. So this test issues NO bus
# RPC after boot and reads audit.log straight off disk (a file read, not an
# RPC, so it can't itself drive a tick).
#
# Setup: seed a wedged-tool events.jsonl (a turn opened + a Bash PreToolUse,
# NO PostToolUse / Stop) BEFORE boot, with a tiny tool budget. The boot tick
# computes open_tool_since + budget and arms the timerfd; the timer fires
# ~budget later → tick → maybeEscalateStuck emits the tool-wedged alarm. If
# escalation only fired on RPC ticks, the alarm would never appear here.

set -uo pipefail

BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/timerfd-itest.XXXXXX)"
export CLAUDE_BUS_TOOL_BUDGET_MS=2000      # wedged-tool budget
export CLAUDE_BUS_STUCK_BUDGET_MS=600000   # keep turn-stuck far away
export CLAUDE_BUS_ESCALATE_NO_PANE_GATE=1  # no zellij to resolve a pane in CI
AUDIT="$CLAUDE_BUS_STATE/topics/audit.log"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }
# Count the alarm in the binary topic log WITHOUT an RPC (-a: treat as text).
alarms() { grep -ac 'tool-wedged agent=wedgey' "$AUDIT" 2>/dev/null || echo 0; }

TS="$(date -u +%FT%T.%3NZ)"
cat > "$CLAUDE_BUS_STATE/events.jsonl" <<EOF
{"ts":"$TS","session":"t","agent":"wedgey","pane":"9","event":"UserPromptSubmit","payload":{"prompt":"do a thing"}}
{"ts":"$TS","session":"t","agent":"wedgey","pane":"9","event":"PreToolUse","payload":{"tool_name":"Bash"}}
EOF

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID); NO RPC will be issued from here"

# Just after boot, before the budget elapses: the deadline is in the future,
# so no alarm yet. (Pure disk read.)
sleep 0.4
ck "$(alarms)" "0" "no alarm before the tool budget elapses"

# Wait WELL past the 2s budget — issuing ZERO bus commands — then read the
# audit log off disk. The timerfd is the only thing that can have ticked the
# broker.
sleep 4
n="$(alarms)"
ck "$([ "$n" -ge 1 ] && echo fired)" "fired" \
   "tool-wedged alarm fired on the timerfd alone, zero RPC (count=$n)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mTIMERFD QUIET-FLEET CHECK PASSED\033[0m"
else
  echo -e "\033[1mTIMERFD QUIET-FLEET CHECK FAILED\033[0m"
  echo "broker.out:"; cat "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
