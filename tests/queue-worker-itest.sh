#!/usr/bin/env bash
# queue-worker-itest.sh — integration test for the `bus queue` push/pop feature
# exercising a realistic slow-consumer / work-queue pattern:
#   - multiple producers push tasks into a work-queue topic;
#   - one worker pops ONE AT A TIME, writes output, sleeps 3s, then pops next;
#   - asserts one-at-a-time pacing (not instant drain), FIFO order, completeness,
#     and empty-queue rc=1 at the end.
# Hermetic: isolated CLAUDE_BUS_STATE, broker cleaned up on EXIT.

set -uo pipefail

BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE
CLAUDE_BUS_STATE="$(mktemp -d /tmp/queue-worker-itest.XXXXXX)"
OUT="$CLAUDE_BUS_STATE/worker-output.txt"
AUDIT="$CLAUDE_BUS_STATE/broker.out"
fail=0

note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ck() {  # ck ACTUAL EXPECTED LABEL
  if [ "$1" = "$2" ]; then echo "  ok: $3";
  else echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi
}
ck_ge() {  # ck_ge ACTUAL MIN LABEL
  if [ "$1" -ge "$2" ]; then echo "  ok: $3 ($1 >= $2)";
  else echo "  FAIL: $3 (got $1, want >= $2)"; fail=1; fi
}
ck_le() {  # ck_le ACTUAL MAX LABEL
  if [ "$1" -le "$2" ]; then echo "  ok: $3 ($1 <= $2)";
  else echo "  FAIL: $3 (got $1, want <= $2)"; fail=1; fi
}

# Minimal fake-zellij so paneId() and sendToPaneSafe don't fail loudly
# when broker delivery tries to resolve pane names in headless mode.
FAKE="$CLAUDE_BUS_STATE/fakebin"; mkdir -p "$FAKE"
cat > "$FAKE/zellij" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *list-panes*) printf '{"tabs":[]}\n' ;;
  *) : ;;
esac
exit 0
EOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

# Start an isolated broker (NOT nohup/setsid/disown per broker-spec).
"$BUS" broker run >"$AUDIT" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT

# Wait up to 5s for the broker socket to appear.
for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break
  sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up; output:"; cat "$AUDIT"; exit 1
fi
echo "isolated broker up (pid $BPID)"

# ─── Step 1: PRODUCERS — push 4 tasks upfront ───────────────────────────────
note "1. producers push 4 tasks into work-queue 'jobs'"

# Push all 4 sequentially to guarantee FIFO push-order. The spec allows
# "just 4 sequential bus queue push" — concurrent producers would race and
# produce non-deterministic order in the queue (the work-queue topic is a
# FIFO by push arrival, not by origin). We verify FIFO from the consumer
# side, so the push order must be deterministic.
for i in 1 2 3 4; do
  "$BUS" queue push jobs "job-$i" >/dev/null
done

# Confirm the queue appears in bus queue list.
list_out="$("$BUS" queue list)"
if printf '%s\n' "$list_out" | grep -q '^jobs$'; then
  echo "  ok: 'jobs' appears in queue list"
else
  echo "  FAIL: 'jobs' not in queue list (got: $list_out)"; fail=1
fi

# ─── Step 2: WORKER — slow consumer loop in the background ──────────────────
note "2. starting slow worker (pop → sleep 3s loop)"

# The worker: pop one item, write it to $OUT, sleep 3s, repeat.
# `bus queue pop` returns rc=1 + empty stdout when the queue is drained.
(
  while item=$("$BUS" queue pop jobs 2>/dev/null) && [ -n "$item" ]; do
    printf '%s\n' "$item" >> "$OUT"
    sleep 3
  done
) &
WPID=$!

# ─── Step 3: PACING ASSERTION — one-at-a-time, not instant drain ────────────
note "3. pacing assertion: ~1 item per 3s (not instant drain)"

# After ~1s the worker has had time to pop and write the first item
# but not yet finish the 3s sleep for job-1, so only 1 line should be present.
sleep 1

lines_at_1s=$(wc -l < "$OUT" 2>/dev/null || echo 0)
echo "  lines at ~1s: $lines_at_1s"
ck_ge "$lines_at_1s" 1 "at least 1 line popped by 1s"
ck_le "$lines_at_1s" 1 "at most 1 line by 1s (slow pacing, not instant drain)"

# At ~3.5s the first item's sleep is done and the second should be mid-sleep.
sleep 2.5
lines_at_3_5s=$(wc -l < "$OUT" 2>/dev/null || echo 0)
echo "  lines at ~3.5s: $lines_at_3_5s"
ck_ge "$lines_at_3_5s" 1 "at least 1 line by 3.5s"
ck_le "$lines_at_3_5s" 2 "at most 2 lines by 3.5s (second item mid-sleep)"

# ─── Step 4: COMPLETENESS + FIFO ────────────────────────────────────────────
note "4. completeness + FIFO order (waiting up to ~14s total)"

# Give the worker enough time to process all 4 × 3s + startup slack.
# We already spent ~3.5s above; wait up to ~11 more seconds.
DEADLINE=$(( $(date +%s) + 11 ))
while [ "$(wc -l < "$OUT" 2>/dev/null || echo 0)" -lt 4 ]; do
  [ "$(date +%s)" -ge "$DEADLINE" ] && break
  sleep 0.5
done
wait "$WPID" 2>/dev/null || true

total_lines=$(wc -l < "$OUT" 2>/dev/null || echo 0)
ck "$total_lines" "4" "all 4 jobs completed"

# Verify FIFO order: job-1 job-2 job-3 job-4 in sequence.
if [ -f "$OUT" ]; then
  line1=$(sed -n '1p' "$OUT")
  line2=$(sed -n '2p' "$OUT")
  line3=$(sed -n '3p' "$OUT")
  line4=$(sed -n '4p' "$OUT")
  ck "$line1" "job-1" "line 1 is job-1 (FIFO)"
  ck "$line2" "job-2" "line 2 is job-2 (FIFO)"
  ck "$line3" "job-3" "line 3 is job-3 (FIFO)"
  ck "$line4" "job-4" "line 4 is job-4 (FIFO)"
else
  echo "  FAIL: output file missing"; fail=1
fi

# ─── Step 5: EMPTY QUEUE ASSERTION ──────────────────────────────────────────
note "5. queue is empty after worker drains it"

"$BUS" queue pop jobs >/dev/null 2>&1
rc=$?
ck "$rc" "1" "bus queue pop on drained queue returns rc=1"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mQUEUE WORKER INTEGRATION TEST PASSED\033[0m"
else
  echo -e "\033[1mQUEUE WORKER INTEGRATION TEST FAILED\033[0m"
  echo "broker.out tail:"; tail -20 "$AUDIT" 2>/dev/null
fi
exit "$fail"
