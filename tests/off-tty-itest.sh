#!/usr/bin/env bash
# off-tty-itest.sh — end-to-end integration test for OFF-TTY delivery,
# now the FLEET DEFAULT (roadmap 2.1, Fork B). Spins up an ISOLATED
# broker in a throwaway $STATE (never the live one) and proves:
#   - every agent is off-TTY by DEFAULT (drain -> additionalContext);
#   - the durable TTY opt-out set (compiled-in "comms" + the
#     $CLAUDE_BUS_TTY_AGENTS env list) stays on the push path (drain
#     returns empty — no double-delivery);
#   - presence + consume-as-ack semantics intact.
# Cleans up the broker + state on exit. Touches nothing live.

set -uo pipefail

# Resolve the binary from THIS tree (workspace-relative), not a hardcoded
# default-tree path — so a workspace build is tested, not the landed one.
BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/off-tty-itest.XXXXXX)"
export CLAUDE_BUS_TTY_AGENTS="tester2"  # env opt-out (comms is compiled-in)
fail=0

note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ck() {  # ck ACTUAL EXPECTED LABEL
  if [ "$1" = "$2" ]; then echo "  ok: $3"; else
    echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi
}

# Isolated broker — inherits CLAUDE_BUS_TTY_AGENTS so its drain RPC sees
# the opt-out list. NOT nohup/setsid/disown.
"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT

for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break
  sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up; output:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID), state $CLAUDE_BUS_STATE"

note "1. a normal agent is off-TTY by DEFAULT — no flag needed"
"$BUS" msg mail tester "first off-tty message" >/dev/null
"$BUS" msg mail tester "second one" >/dev/null

note "2. drain -> additionalContext JSON carrying BOTH (no TTY write)"
out="$("$BUS" msg drain tester UserPromptSubmit)"
echo "$out"
echo "$out" | grep -q '"hookEventName":"UserPromptSubmit"'; ck "$?" 0 "emits additionalContext"
echo "$out" | grep -q 'first off-tty message'; ck "$?" 0 "carries first record"
echo "$out" | grep -q 'second one'; ck "$?" 0 "carries second record"

note "3. drain again -> cursor advanced on consume = the ack; nothing left"
out2="$("$BUS" msg drain tester)"
ck "$out2" "" "second drain empty (consume advanced the cursor)"

note "4. presence gate: attach, mail, drain DEFERS; detach delivers"
mkdir -p "$CLAUDE_BUS_STATE/presence"; touch "$CLAUDE_BUS_STATE/presence/tester"
"$BUS" msg mail tester "typed while you held the keyboard" >/dev/null
out3="$("$BUS" msg drain tester)"; ck "$out3" "" "drain defers while attached"
rm -f "$CLAUDE_BUS_STATE/presence/tester"
out4="$("$BUS" msg drain tester)"
echo "$out4" | grep -q 'typed while you held the keyboard'; ck "$?" 0 "deferred record delivers after detach"

note "5. TTY OPT-OUT (compiled-in 'comms') stays on the push path"
"$BUS" msg mail comms "comms is human-facing" >/dev/null
out5="$("$BUS" msg drain comms)"
ck "$out5" "" "comms drain empty (stays on TTY push, no double-delivery)"

note "6. TTY OPT-OUT via \$CLAUDE_BUS_TTY_AGENTS (tester2) stays on push"
"$BUS" msg mail tester2 "env opt-out" >/dev/null
out6="$("$BUS" msg drain tester2)"
ck "$out6" "" "env-listed agent drains nothing"

note "7. per-agent isolation: another default agent drains only its own"
"$BUS" msg mail tester3 "for tester3 only" >/dev/null
out7="$("$BUS" msg drain tester3)"
echo "$out7" | grep -q 'for tester3 only'; ck "$?" 0 "tester3 drains its own record"
echo "$out7" | grep -q 'off-tty message'; ck "$?" 1 "tester3 does NOT see tester's mail"

echo
if [ "$fail" = 0 ]; then echo -e "\033[1mALL OFF-TTY INTEGRATION CHECKS PASSED\033[0m"; else
  echo -e "\033[1mSOME CHECKS FAILED\033[0m"; fi
exit "$fail"
