#!/usr/bin/env bash
# c2b-drain-wedge-itest.sh — C2b regression: drain cursor permanent wedge.
#
# DEFECT (pre-fix):
#   noteDrainDelivery set next_retry_at=0, so scanRetries never fired on
#   drain-delivered entries. One lost bus-ack wedged that record forever —
#   the cursor never advanced and the in-flight entry never expired.
#
# FIX:
#   noteDrainDelivery now sets next_retry_at = dispatched_at_ms + ackTimeoutMs().
#   scanRetries treats drain_origin entries differently on deadline: instead of
#   TTY-re-dispatching (no pane), it forgets the in-flight entry so the next
#   drain re-reads the un-advanced cursor and re-delivers (at-least-once).
#   Attempts accumulate across drain cycles (persisted in the in-flight sidecar)
#   so at kMaxAttempts the entry escalates rather than looping forever.
#
# SCENARIO A — re-delivery after lost ack:
#   1. Enqueue rec0.
#   2. Raw drain RPC (no auto-ack written) — rec0 in-flight.
#   3. Wait past ack deadline — scanRetries forgets in-flight, cursor stays put.
#   4. Second raw drain — rec0 should be re-delivered (at-least-once).
#
# SCENARIO B — escalation after kMaxAttempts lost acks:
#   5. Continue from re-delivered state (new in-flight, attempts=2).
#   6. Wait past deadline again — scanRetries should hit kMaxAttempts and
#      escalate (audit + inbox-ops).
#   7. Verify escalation record in audit or inbox-ops topic.
#
# Uses raw nc -U RPC calls (no auto-ack), CLAUDE_BUS_ACK_TIMEOUT_MS=400ms so
# the test completes quickly.
#
# Gotchas (project_broker_itest_gotchas):
#   - assert on broker output, not racy client read
#   - `grep -c || true` to avoid exit-1 on no-match
#   - wait only on named pids
#   - bare `wait` blocks on background broker

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/c2b-wedge-itest.XXXXXX)"
export CLAUDE_BUS_ACK_TIMEOUT_MS=400
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

# Direct drain RPC — no bus-ack written (simulates a lost ack).
raw_drain() {
  printf '{"op":"drain","agent":"%s"}\n' "$1" | nc -U "$SOCK" 2>/dev/null
}

# Drive broker ticks so scanRetries fires.
tick() { "$BUS" state >/dev/null 2>&1; }
wait_past_deadline() {
  sleep 0.6  # > CLAUDE_BUS_ACK_TIMEOUT_MS (400ms)
  for _ in $(seq 1 8); do tick; sleep 0.05; done
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

# ── SCENARIO A ───────────────────────────────────────────────────────────────
echo
echo "=== SCENARIO A: re-delivery after lost ack ==="

echo "1. enqueue rec0"
ID0="$("$BUS" msg enqueue "inbox-${AGENT}" "wedge-probe-$$" 2>/dev/null)"
echo "   rec0=$ID0"

echo "2. first drain (raw RPC, no auto-ack) — rec0 in-flight"
drain1="$(raw_drain "$AGENT")"
got1=$(echo "$drain1" | grep -c "wedge-probe-$$" || true)
ck "$got1" "1" "drain1 delivers rec0"

# Confirm in-flight entry created
inflight_before=$(ls "$CLAUDE_BUS_STATE/in-flight/" 2>/dev/null | grep -c "." || true)
ck "$([ "$inflight_before" -ge 1 ] && echo yes || echo no)" "yes" "in-flight entry created after drain"

echo "3. wait past ack deadline — scanRetries should forget in-flight (not TTY-push)"
wait_past_deadline

# Pre-fix: in-flight entry survives forever (next_retry_at=0 → scanRetries ignores it).
# Post-fix: in-flight entry removed from memory (file may linger as attempts sidecar).
inflight_after=$(ls "$CLAUDE_BUS_STATE/in-flight/" 2>/dev/null | grep "^${ID0}" | grep -c "." || true)
# The sidecar may still exist on disk (that's intentional: it carries the attempts count).
# What matters is that the NEXT drain re-delivers (cursor not advanced).
echo "   in-flight files after deadline: $(ls "$CLAUDE_BUS_STATE/in-flight/" 2>/dev/null | wc -l | tr -d ' ')"

echo "4. second raw drain — rec0 MUST be re-delivered (at-least-once)"
drain2="$(raw_drain "$AGENT")"
got2=$(echo "$drain2" | grep -c "wedge-probe-$$" || true)
ck "$got2" "1" "rec0 re-delivered after lost ack (cursor not stuck)"

# ── SCENARIO B ───────────────────────────────────────────────────────────────
echo
echo "=== SCENARIO B: escalation after kMaxAttempts (3) lost acks ==="
# At this point: rec0 was re-delivered (drain2), so it's in-flight again with
# attempts=2 (carried from the sidecar). Wait past the deadline again →
# scanRetries increments to attempts=3 >= kMaxAttempts → escalate.
#
# kMaxAttempts = 3 (delivery.h), so: initial drain(1) + re-deliver(2) + re-deliver(3).
# The attempts counter accumulates across drain cycles via the sidecar file.

echo "5. second drain (drain2) already captured above; now wait past deadline again"
wait_past_deadline

echo "6. third drain to hit kMaxAttempts — should escalate, NOT re-deliver"
drain3="$(raw_drain "$AGENT")"
got3=$(echo "$drain3" | grep -c "wedge-probe-$$" || true)
# At kMaxAttempts the broker escalates and advances the cursor, so drain3 may
# deliver rec0 one last time OR return empty depending on the escalation path.
# The critical check is that escalation appears in audit/inbox-ops.
echo "   drain3 delivered: $got3 (escalation takes priority at kMaxAttempts)"

# Wait for the escalation to land in the audit topic.
for _ in $(seq 1 5); do tick; sleep 0.1; done

echo "7. verify escalation in audit or inbox-ops"
audit="$("$BUS" msg peek audit --limit 200 2>/dev/null)"
ops="$("$BUS" msg peek inbox-ops --limit 200 2>/dev/null)"
esc_audit=$(echo "$audit" | grep -c "max attempts\|drain.*no bus-ack\|delivery exhausted" || true)
esc_ops=$(echo "$ops" | grep -c "exhausted\|drain.*no bus-ack" || true)
echo "   audit escalation lines: $esc_audit"
echo "   inbox-ops escalation lines: $esc_ops"
ck "$([ "${esc_audit:-0}" -ge 1 ] || [ "${esc_ops:-0}" -ge 1 ] && echo escalated || echo none)" \
   "escalated" \
   "escalation recorded after kMaxAttempts drain cycles with no bus-ack"

echo
if [ "$fail" = 0 ]; then
  echo "C2B DRAIN WEDGE REGRESSION PASSED"
else
  echo "C2B DRAIN WEDGE REGRESSION FAILED"
  echo "broker.out tail:"; tail -20 "$CLAUDE_BUS_STATE/broker.out"
  echo "in-flight dir:"; ls -la "$CLAUDE_BUS_STATE/in-flight/" 2>/dev/null || echo "(empty)"
fi
exit "$fail"
