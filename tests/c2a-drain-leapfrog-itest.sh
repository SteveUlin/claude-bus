#!/usr/bin/env bash
# c2a-drain-leapfrog-itest.sh — C2a regression: drain cursor leapfrog.
#
# DEFECT (pre-fix):
#   Drain handler loop: with [rec0 in-flight, rec1 ttl-expired] at the cursor,
#   rec0's already_inflight skip was correct, but rec1's ttl-expired skip set
#   advance_cursor = cursorAfter(rec1), leapfrogging the cursor PAST rec0.
#   If rec0's bus-ack was lost, rec0 was silently consumed without delivery or
#   escalation. The advance_blocked flag latches on the first in-flight record
#   so no later skippable record can advance past it.
#
# SCENARIO:
#   1. Enqueue rec0 (no TTL) and rec1 (very short TTL).
#   2. First drain (raw RPC — no auto-ack): rec0 delivered and marked in-flight.
#   3. Wait past rec1's TTL.
#   4. Second drain without acking rec0: rec1 is now TTL-expired, rec0 in-flight.
#      Pre-fix: cursor leapfrogs past both. Post-fix: advance_blocked prevents
#      any cursor advance; drain returns nothing.
#   5. Let rec0's ack deadline pass (no bus-ack emitted).
#   6. Third drain: rec0 should be re-delivered (cursor not leapfrogged).
#
# Uses raw nc -U RPC calls (bypassing bus msg drain's auto-ack), so the
# broker's in-flight map holds rec0 without an ack — exactly the leapfrog
# precondition.
#
# Gotchas (project_broker_itest_gotchas):
#   - assert on broker state/output, not racy client read
#   - `grep -c || true` to avoid exit-1 from grep on no-match
#   - `wait` only on named pids

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/c2a-leapfrog-itest.XXXXXX)"
# Ack timeout long enough that it doesn't fire during the TTL-expiry window
# (step 3), but short enough that step 5 runs quickly.
export CLAUDE_BUS_ACK_TIMEOUT_MS=1500
AGENT="alice"
SOCK="$CLAUDE_BUS_STATE/broker.sock"
fail=0
ck() {
  if [ "$1" = "$2" ]; then
    echo "  ok: $3"
  else
    echo "  FAIL: $3 (got [$1] want [$2])"
    fail=1
  fi
}

# Call the broker drain RPC directly — NO bus-ack written (unlike bus msg drain).
# readLine on the server side needs a terminating '\n' to know the message ended.
raw_drain() {
  printf '{"op":"drain","agent":"%s"}\n' "$1" | nc -U "$SOCK" 2>/dev/null
}

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do
  [ -S "$SOCK" ] && break; sleep 0.1
done
if [ ! -S "$SOCK" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID), ack-timeout=${CLAUDE_BUS_ACK_TIMEOUT_MS}ms"

# Drive broker ticks (scanRetries, scanEvents).
tick() { "$BUS" state >/dev/null 2>&1; }

# ── Step 1: enqueue rec0 (no TTL) and rec1 (100ms TTL) ──────────────────────
echo "1. enqueue rec0 (no TTL) and rec1 (100ms TTL)"
ID0="$("$BUS" msg enqueue "inbox-${AGENT}" "rec0-payload" 2>/dev/null)"
ID1="$("$BUS" msg enqueue "inbox-${AGENT}" "rec1-payload" --ttl 100 2>/dev/null)"
echo "   rec0=$ID0  rec1=$ID1"

# ── Step 2: first drain (raw RPC — no bus-ack written) ───────────────────────
# rec0 should be delivered and marked in-flight; rec1 not yet TTL-expired.
echo "2. first drain (raw RPC, no auto-ack) — rec0 in-flight"
drain1="$(raw_drain "$AGENT")"
echo "   drain1 result: $(echo "$drain1" | grep -o '"body":"[^"]*"' | head -1)"
got_rec0=$(echo "$drain1" | grep -c "rec0-payload" || true)
ck "$got_rec0" "1" "drain1 delivers rec0"

# ── Step 3: wait past rec1's TTL ─────────────────────────────────────────────
# rec1 TTL=100ms; ack timeout=1500ms — plenty of margin before scanRetries
# would fire on rec0.
echo "3. wait 200ms for rec1 TTL to expire (well before ack timeout)"
sleep 0.2
for _ in $(seq 1 3); do tick; done  # drive broker ticks

# ── Step 4: second drain without acking rec0 ─────────────────────────────────
# rec0 is still in-flight (no bus-ack). rec1 is now TTL-expired.
# Pre-fix: cursor leaps to after rec1, consuming rec0 silently.
# Post-fix: advance_blocked latches on rec0; cursor stays put; empty result.
echo "4. second drain without acking rec0 (rec0 in-flight, rec1 expired)"
drain2="$(raw_drain "$AGENT")"
msgs2=$(echo "$drain2" | grep -c '"body"' || true)
echo "   drain2 message count: $msgs2"
ck "$msgs2" "0" "second drain returns nothing (advance_blocked prevents leapfrog)"

# ── Step 5: wait past ack deadline without acking ────────────────────────────
echo "5. wait past ack deadline (${CLAUDE_BUS_ACK_TIMEOUT_MS}ms) without acking"
sleep 1.8  # > CLAUDE_BUS_ACK_TIMEOUT_MS (1500ms)
for _ in $(seq 1 10); do tick; sleep 0.05; done  # drive scanRetries

# ── Step 6: third drain — rec0 must be re-delivered ──────────────────────────
# C2b fix (noteDrainDelivery has a real deadline): scanRetries removed the
# in-flight entry after the deadline, cursor was NOT advanced (no leapfrog),
# so the next drain re-reads the cursor and re-delivers rec0.
echo "6. third drain — rec0 must be re-delivered (cursor not leapfrogged)"
drain3="$(raw_drain "$AGENT")"
got_rec0_again=$(echo "$drain3" | grep -c "rec0-payload" || true)
ck "$got_rec0_again" "1" "rec0 re-delivered on third drain (cursor intact)"

echo
if [ "$fail" = 0 ]; then
  echo "C2A LEAPFROG REGRESSION PASSED"
else
  echo "C2A LEAPFROG REGRESSION FAILED"
  echo "broker.out tail:"; tail -20 "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
