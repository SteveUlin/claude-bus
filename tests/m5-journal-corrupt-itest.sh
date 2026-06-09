#!/usr/bin/env bash
# m5-journal-corrupt-itest.sh — M5 regression: journal corruption is invisible.
#
# DEFECT (pre-fix):
#   Journal::peek / dump / tail accept a truncated_at out-param but all
#   production call-sites passed nullptr — corruption was silently read as
#   a clean empty log (records just stopped at the bad byte). The delivery
#   path would stall permanently with no operator signal.
#
# FIX:
#   All delivery-path peek / drain / dispatch calls now pass a real &trunc.
#   When trunc >= 0, the broker calls Loop::reportJournalCorrupt (per-topic
#   latch, fires once per broker run) which calls emitAudit("journal-corrupt")
#   so the operator sees the event in the audit topic. The broker's drain RPC
#   handler also surfaces truncated_at in-band in the response.
#
# SCENARIO:
#   1. Start an isolated broker, enqueue a record (creates the journal file).
#   2. Truncate the journal file mid-record (removes last 10 bytes).
#   3. Call the broker drain RPC (raw nc -U) — it peeks the corrupted log.
#   4. Assert "journal-corrupt" appears in the audit topic.
#   5. Assert the per-topic latch: a second drain on the same topic does NOT
#      produce a second audit line (one audit per topic per broker run).
#
# Gotchas (project_broker_itest_gotchas):
#   - assert on audit topic, not racy broker.out
#   - `grep -c || true` to avoid exit-1 on no-match
#   - wait only on named pids

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/m5-journal-corrupt-itest.XXXXXX)"
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

raw_drain() {
  printf '{"op":"drain","agent":"%s"}\n' "$1" | nc -U "$SOCK" 2>/dev/null
}
tick() { "$BUS" state >/dev/null 2>&1; }

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do
  [ -S "$SOCK" ] && break; sleep 0.1
done
if [ ! -S "$SOCK" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID)"

# ── Step 1: enqueue a record to create the journal file ───────────────────────
echo "1. enqueue a record to inbox-alice (creates inbox-alice.log)"
"$BUS" msg enqueue "inbox-alice" "healthy-record" >/dev/null 2>&1
sleep 0.2

JF="$CLAUDE_BUS_STATE/topics/inbox-alice.log"
if [ ! -f "$JF" ]; then
  echo "FAIL: journal file not created at $JF"; exit 1
fi
ORIG_SIZE="$(stat -c %s "$JF")"
echo "   journal size: $ORIG_SIZE bytes"

# ── Step 2: truncate last 10 bytes to corrupt the record ──────────────────────
echo "2. corrupt journal (truncate last 10 bytes mid-record)"
CORRUPT_SIZE=$((ORIG_SIZE - 10))
truncate -s "$CORRUPT_SIZE" "$JF"
echo "   truncated from $ORIG_SIZE to $CORRUPT_SIZE bytes"

# ── Step 3: call drain RPC — peek detects corruption ─────────────────────────
echo "3. drain RPC on corrupted log (no auto-ack)"
drain1="$(raw_drain "alice")"
echo "   drain response: $drain1"
# The corrupt record appears as empty — no messages delivered (truncated_at fires).
msgs1=$(echo "$drain1" | grep -c '"body"' || true)
ck "$msgs1" "0" "drain returns empty on corrupted log (corrupt record not delivered)"

# Drive ticks to ensure emitAudit lands.
for _ in $(seq 1 5); do tick; sleep 0.05; done

# ── Step 4: assert audit contains "journal-corrupt" ───────────────────────────
echo "4. assert journal-corrupt audit line"
audit="$("$BUS" msg peek audit --limit 200 2>/dev/null)"
corrupt_lines=$(echo "$audit" | grep -c "journal-corrupt" || true)
ck "$([ "${corrupt_lines:-0}" -ge 1 ] && echo yes || echo no)" "yes" \
   "journal-corrupt audit line present (lines=$corrupt_lines)"

# Also check the audit body includes the topic name.
has_topic=$(echo "$audit" | grep -c "topic=inbox-alice" || true)
ck "$([ "${has_topic:-0}" -ge 1 ] && echo yes || echo no)" "yes" \
   "audit body includes topic=inbox-alice"

# ── Step 5: per-topic latch — second drain must NOT add another audit line ────
echo "5. second drain — corrupt_reported_ latch prevents duplicate audit"
raw_drain "alice" >/dev/null 2>&1
for _ in $(seq 1 5); do tick; sleep 0.05; done

audit2="$("$BUS" msg peek audit --limit 200 2>/dev/null)"
corrupt_lines2=$(echo "$audit2" | grep -c "journal-corrupt" || true)
ck "$corrupt_lines2" "$corrupt_lines" \
   "latch: second drain does NOT produce a second journal-corrupt audit line"

echo
if [ "$fail" = 0 ]; then
  echo "M5 JOURNAL CORRUPT REGRESSION PASSED"
else
  echo "M5 JOURNAL CORRUPT REGRESSION FAILED"
  echo "broker.out tail:"; tail -20 "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
