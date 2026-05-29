#!/usr/bin/env bash
# off-tty-itest.sh — end-to-end integration test for the OFF-TTY
# delivery path (roadmap 2.1). Spins up an ISOLATED broker in a throwaway
# $STATE (never the live one), proves a mailed record arrives via
# drain -> additionalContext (NOT a TTY write), with the presence
# sentinel and consume-as-ack semantics intact. Cleans up the broker +
# state on exit.
#
# Does NOT touch the live broker, fleet, or settings.json. Run directly:
#   tests/off-tty-itest.sh

set -uo pipefail

BUS="/home/sulin/claude-bus/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/off-tty-itest.XXXXXX)"
fail=0

note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ck() {  # ck ACTUAL EXPECTED LABEL
  if [ "$1" = "$2" ]; then
    echo "  ok: $3"
  else
    echo "  FAIL: $3 (got [$1] want [$2])"
    fail=1
  fi
}

# --- isolated broker (PR_SET_PDEATHSIG ties it to this script; we also
# --- kill it explicitly). NOT nohup/setsid/disown — those would orphan.
"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT

for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break
  sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up; output:"
  cat "$CLAUDE_BUS_STATE/broker.out"
  exit 1
fi
echo "isolated broker up (pid $BPID), state $CLAUDE_BUS_STATE"

note "1. flag 'tester' off-TTY, then mail two records"
mkdir -p "$CLAUDE_BUS_STATE/off-tty"
touch "$CLAUDE_BUS_STATE/off-tty/tester"
"$BUS" msg mail tester "first off-tty message" >/dev/null
"$BUS" msg mail tester "second one" >/dev/null

note "2. drain -> additionalContext JSON carrying BOTH (no TTY write)"
out="$("$BUS" msg drain tester UserPromptSubmit)"
echo "$out"
echo "$out" | grep -q '"hookEventName":"UserPromptSubmit"'
ck "$?" 0 "emits hookSpecificOutput.additionalContext"
echo "$out" | grep -q 'first off-tty message'
ck "$?" 0 "additionalContext carries first record"
echo "$out" | grep -q 'second one'
ck "$?" 0 "additionalContext carries second record"

note "3. drain again -> cursor advanced on consume = the ack; nothing left"
out2="$("$BUS" msg drain tester)"
ck "$out2" "" "second drain is empty (consume advanced the cursor)"

note "4. presence gate: attach, mail, drain must DEFER"
mkdir -p "$CLAUDE_BUS_STATE/presence"
touch "$CLAUDE_BUS_STATE/presence/tester"
"$BUS" msg mail tester "typed while you held the keyboard" >/dev/null
out3="$("$BUS" msg drain tester)"
ck "$out3" "" "drain defers while [bus-attach] sentinel is fresh"

note "5. detach -> the deferred record now delivers"
rm -f "$CLAUDE_BUS_STATE/presence/tester"
out4="$("$BUS" msg drain tester)"
echo "$out4" | grep -q 'typed while you held the keyboard'
ck "$?" 0 "deferred record delivers after detach"

note "6. a second flagged agent drains its own inbox independently"
touch "$CLAUDE_BUS_STATE/off-tty/tester2"
"$BUS" msg mail tester2 "for tester2 only" >/dev/null
out5="$("$BUS" msg drain tester2)"
echo "$out5" | grep -q 'for tester2 only'
ck "$?" 0 "tester2 drains its own record"
echo "$out5" | grep -q 'off-tty message'
ck "$?" 1 "tester2 does NOT see tester's mail (per-agent isolation)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mALL OFF-TTY INTEGRATION CHECKS PASSED\033[0m"
else
  echo -e "\033[1mSOME CHECKS FAILED\033[0m"
fi
exit "$fail"
