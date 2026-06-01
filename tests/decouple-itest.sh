#!/usr/bin/env bash
# decouple-itest.sh — Pillar D / W23 proof. The intake/processing split means
# a slow PROCESSING tick (blocked in a capped zellij fork) can NEVER stop the
# INTAKE thread from accept()ing — connections are always taken; a full queue
# is shed with a "broker busy" reply, not a wedge / connection-refusal.
#
# Setup: a FAKE zellij on PATH that sleeps on every call, so each delivery
# tick blocks the processing thread for the full 5 s subprocess cap. One
# seeded agent makes the tick fork zellij (paneId). With CLAUDE_BUS_RPC_QUEUE_MAX=1,
# RPCs fired while processing is parked in that fork pile up past the bound and
# the overflow is rejected with "broker busy, retry" — observable proof that
# intake stayed live + shed load instead of wedging.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="$ROOT/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/decouple-itest.XXXXXX)"
export CLAUDE_BUS_RPC_QUEUE_MAX=1   # tiny bound so a parked tick overflows it
FAKE="$CLAUDE_BUS_STATE/fakebin"
mkdir -p "$FAKE"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# Fake zellij: sleep long on every invocation so the tick's first list-panes
# fork parks the processing thread for the whole 5 s cap.
cat > "$FAKE/zellij" <<'EOF'
#!/usr/bin/env bash
sleep 8
exit 0
EOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

# One agent so readAgents() is non-empty and the tick actually forks zellij.
TS="$(date -u +%FT%T.%3NZ)"
cat > "$CLAUDE_BUS_STATE/events.jsonl" <<EOF
{"ts":"$TS","session":"t","agent":"sleepy","pane":"9","event":"UserPromptSubmit","payload":{"prompt":"hi"}}
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
echo "isolated broker up (pid $BPID); processing thread parked in a 5 s fork"

# The boot tick is already forking the fake (sleeping) zellij, so the
# processing thread is parked. Fire a burst of RPCs concurrently: intake
# accepts every one (never refused), enqueues the first, and sheds the rest
# with "broker busy" once the 1-deep queue is full.
: > "$CLAUDE_BUS_STATE/burst.out"
pids=()
for _ in $(seq 1 8); do
  ( out="$("$BUS" topic list 2>&1)"
    if printf '%s' "$out" | grep -qi "connect\|refused\|No such file or directory"; then
      echo refused; else echo served; fi ) >> "$CLAUDE_BUS_STATE/burst.out" &
  pids+=($!)
done
# Wait ONLY on the burst subshells — a bare `wait` would also block on the
# broker (a background child of this shell) which never exits on its own.
wait "${pids[@]}"

# Assert on the BROKER's own shed log (deterministic, no client-side race):
# every backpressure reject logs "queue full — shed" to stderr. grep -c always
# prints a number, so `|| true` (not `|| echo 0`, which double-prints).
shed=$(grep -c "queue full" "$CLAUDE_BUS_STATE/broker.out" || true)
refused=$(grep -c refused "$CLAUDE_BUS_STATE/burst.out" || true)

ck "$([ "${shed:-0}" -ge 1 ] && echo shed)" "shed" \
   "intake shed >=1 RPC with 'broker busy' while the tick was parked (broker shed log=$shed)"
ck "${refused:-0}" "0" \
   "no connection was refused/reset — intake kept accepting (refused=$refused)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mDECOUPLE / BACKPRESSURE CHECK PASSED\033[0m"
else
  echo -e "\033[1mDECOUPLE / BACKPRESSURE CHECK FAILED\033[0m"
  echo "burst.out:"; cat "$CLAUDE_BUS_STATE/burst.out"
  echo "broker.out:"; cat "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
