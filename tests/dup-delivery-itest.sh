#!/usr/bin/env bash
# dup-delivery-itest.sh — DETERMINISTIC regression for the DUPLICATE-DELIVERY
# broker bug (sulin-prioritized): a record delivered to a recipient TWICE
# (report: an auri->comms message arrived byte-identical twice). bast owns this
# failing regression; elodin owns the fix (verification-parallelism).
#
# ── ROOT CAUSE (elodin, confirmed 2026-06-01) ─────────────────────────────
# comms is a hardcoded TTY opt-out (tty_policy.h:26), so dispatchAgentInbox
# does NOT off-tty-return (delivery.cpp:511) -> it's the TTY PUSH path:
# deliverInline -> sendToPaneSafe, marked in-flight with next_retry_at, acked
# by the POSITIONAL UserPromptSubmit join (delivery.cpp:458-490), retried by
# scanRetries (797), up to kMaxAttempts=3.
#
# The ack and the record are TEMPORALLY DECOUPLED — there is no per-record
# correlation. The tick order is scanEvents(874) -> scanRetries(875) ->
# dispatch(882), so:
#   - a UserPromptSubmit consumed by scanEvents when NO in-flight record
#     exists for the agent is BURNED with no effect (oldest_id empty ->
#     line 481 continue), one-shot (events_offset_ advanced); and
#   - a record dispatched in tick N can never be acked by a UPS already
#     consumed in tick N (dispatch runs AFTER scanEvents).
# comms, mid-work, emits no further UPS -> the record stays un-acked ->
# scanRetries re-pushes the SAME bytes after ackTimeout -> byte-identical dup.
# (The off-TTY path already fixed this by acking BY ID — delivery.cpp:434-454,
# whose comment notes it "replaces the positional UserPromptSubmit ack".)
#
# ── THE REGRESSION (elodin's DUP shape) ───────────────────────────────────
# Force the consume-before-dispatch order: append a UPS for comms, drive a
# tick so scanEvents BURNS it (no in-flight yet), THEN mail the record, then
# drive ticks past ackTimeout so scanRetries re-pushes. INVARIANT asserted:
# "a record for which an ack (UPS) was observed is delivered exactly once."
# Pre-fix: the sink sees the record >=2x (FAIL). Post-fix (per-record ack
# correlation): exactly 1x (PASS).
#
# SINK: a fake zellij logs every write-chars (the literal pane delivery);
# count occurrences of the unique payload. >=2 == the dup. (elodin confirmed
# this is the right observable.)
#
# Gotchas baked in (project_broker_itest_gotchas): assert on the fake sink,
# never a racy client read; `grep -c || true`; wait only on named pids.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/dup-delivery-itest.XXXXXX)"
export CLAUDE_BUS_ACK_TIMEOUT_MS=200    # retry comes due fast once un-acked

RCPT="comms"                            # the hardcoded TTY opt-out (push path)
FAKE="$CLAUDE_BUS_STATE/fakebin"
WRITES="$CLAUDE_BUS_STATE/pane-writes.log"
mkdir -p "$FAKE"; : > "$WRITES"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# Fake zellij: synthetic live pane for $RCPT so paneId resolves and
# sendToPaneSafe's dump-screen gate passes, so the push reaches write-chars.
cat > "$FAKE/zellij" <<FAKEEOF
#!/usr/bin/env bash
sub="\$*"
case "\$sub" in
  *"list-panes"*)
    printf '%s\n' '{' '  "id": 1,' '  "is_plugin": false,' '  "title": "$RCPT"' '}'
    ;;
  *"dump-screen"*)
    printf '%s\n' "> " "-- INSERT --  bypass permissions on"
    ;;
  *"write-chars"*)
    printf '%s\n' "WRITE \$*" >> "$WRITES"
    ;;
  *) : ;;
esac
exit 0
FAKEEOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

# Seed a Stop so $RCPT reads as a known, idle agent (push isn't deferred).
TS0="$(date -u +%FT%T.%3NZ)"
printf '%s\n' "{\"ts\":\"$TS0\",\"session\":\"t\",\"agent\":\"$RCPT\",\"pane\":\"1\",\"event\":\"Stop\",\"payload\":{}}" \
  > "$CLAUDE_BUS_STATE/events.jsonl"

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID), recipient=$RCPT (TTY push), ack-timeout=${CLAUDE_BUS_ACK_TIMEOUT_MS}ms"

tick() { "$BUS" state >/dev/null 2>&1; }
PAYLOAD="dup-probe-$$"

# Establish the events_offset_ seek point (first scanEvents seeks to EOF, so
# the seed Stop is never mis-consumed as an ack).
tick; sleep 0.2

echo "1. append a UserPromptSubmit for $RCPT BEFORE any record is in-flight"
TS1="$(date -u +%FT%T.%3NZ)"
printf '%s\n' "{\"ts\":\"$TS1\",\"session\":\"t\",\"agent\":\"$RCPT\",\"pane\":\"1\",\"event\":\"UserPromptSubmit\",\"payload\":{\"prompt\":\"x\"}}" \
  >> "$CLAUDE_BUS_STATE/events.jsonl"
tick; sleep 0.2     # scanEvents BURNS it (oldest_id empty -> no effect)

echo "2. NOW mail the record — dispatched after the ack was already consumed"
"$BUS" msg mail "$RCPT" "$PAYLOAD" >/dev/null
tick; sleep 0.2     # dispatch -> push #1, in-flight, next_retry_at=+200ms

echo "3. drive ticks past ackTimeout — un-acked record gets re-pushed"
for _ in $(seq 1 8); do tick; sleep 0.2; done   # > ackTimeout; let scanRetries run

echo "4. ASSERT exactly-once (a UPS ack WAS observed for $RCPT)"
writes=$(grep -c "$PAYLOAD" "$WRITES" || true)
ck "${writes:-0}" "1" "record delivered to $RCPT EXACTLY once (got $writes; >=2 == the dup bug)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mDUP-DELIVERY REGRESSION PASSED (exactly-once held)\033[0m"
else
  echo -e "\033[1mDUP-DELIVERY REGRESSION FAILED — duplicate delivered (expected on the pre-fix binary)\033[0m"
  echo "pane-writes.log (count=$writes):"; cat "$WRITES"
fi
exit "$fail"
