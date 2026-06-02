#!/usr/bin/env bash
# dup-delivery-itest.sh — DETERMINISTIC repro / regression for the
# DUPLICATE-DELIVERY broker bug (sulin-prioritized): a record delivered to a
# recipient TWICE (concrete report: an auri->comms message arrived byte-
# identical twice). bast owns this failing regression; elodin owns the fix
# (verification-parallelism — the test and the fix touch different files).
#
# ── STATUS: WIP — HARNESS WORKS, FORCING NEEDS ELODIN'S ROOT-CAUSE ─────────
# Empirical (2026-06-01, against the landed binary): the harness drives the
# TTY push path end-to-end and the fake-zellij write-chars sink counts real
# deliveries. As written it reproduces a MULTI-delivery — the same record hit
# the pane 3x. BUT that is under a 50 ms ack-timeout with the UserPromptSubmit
# ack evidently NOT recognized, so it's the retry path re-pushing an
# (apparently) un-acked record — a dup SHAPE, but not yet PROVEN to be the
# specific auri->comms race. The missing piece: force a duplicate where the
# ack IS recognized yet a retry still re-pushes (ack-races-retry), to
# distinguish the true bug from benign at-least-once. That isolation is pinned
# on elodin's root-cause (mailed 2026-06-01, "dup-delivery repro: confirming
# TTY-push path"):
#   Q1 PATH — comms is a COMPILED-IN TTY opt-out (off-tty-itest §5), so the
#     auri->comms dup is on the TTY PUSH path: dispatchAgentInbox ->
#     deliverInline -> sendToPaneSafe, in-flight w/ next_retry_at, acked by
#     UserPromptSubmit positional-join (delivery.cpp:458+), retried by
#     scanRetries. (NOT the off-tty drain/bus-ack path, where re-drain-before-
#     ack is intended at-least-once — off-tty-itest §3.) Confirm.
#   Q2 RACE — true-dup = a scanRetries re-push racing the ack WITHIN a tick:
#     retry-due + UserPromptSubmit-ack pending in the same tick; if scanRetries
#     runs before scanEvents, it re-pushes before the ack removes the in-flight.
#     If that intra-tick ORDER is fixed, the repro is 100% deterministic once
#     retry-due and ack-pending coincide (no sleep-race). Confirm the order /
#     whether a finer window needs a delay-injection point in the ack path.
#   Q3 SINK — this skeleton counts the literal delivery: a fake zellij logs
#     every `write-chars` (the deliverInline write). >=2 writes of the same
#     payload == dup. Confirm, or name a cleaner observable (audit topic?).
#
# Gotchas already baked in (project_broker_itest_gotchas): assert on the
# fake-sink / broker.out, NEVER a racy client classification; `grep -c || true`
# (never `|| echo 0`, which double-prints); wait ONLY on named pids, never a
# bare `wait` (it blocks on the bg broker, a child of this shell).

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Prefer the workspace build; allow BUS override (the workspace bin/ is
# gitignored, so CI / a quick run points this at the default-tree binary).
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/dup-delivery-itest.XXXXXX)"

# Forcing knob #1: make the no-ack retry come due almost immediately, so a
# single tick can hold BOTH a due retry and a pending ack (the Q2 window).
export CLAUDE_BUS_ACK_TIMEOUT_MS=50

# Recipient must be on the TTY PUSH path (Q1). comms is the compiled-in TTY
# opt-out; an env-listed agent works too. Using comms mirrors the live report.
RCPT="comms"

FAKE="$CLAUDE_BUS_STATE/fakebin"
WRITES="$CLAUDE_BUS_STATE/pane-writes.log"   # the SINK: one line per write-chars
mkdir -p "$FAKE"; : > "$WRITES"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# ── Fake zellij (the delivery sink) ───────────────────────────────────────
# sendToPaneSafe (src/pane.cpp) issues, in order:
#   action list-panes --json     -> paneId(): must return a pane for $RCPT
#   action dump-screen --pane-id -> readPaneState(): must look INSERT/idle so
#                                   the not-scrolled/not-locked gates pass
#   action write-chars --pane-id TEXT -> the actual delivery (LOG it)
#   action send-keys  --pane-id ...   -> Enter/Ctrl-U (no-op)
# TODO(finalize): the list-panes JSON + dump-screen content below are best-
# effort; verify field-for-field against listPanesJsonCached() (pane.cpp:470)
# and readPaneState() so the push actually reaches write-chars. If a gate
# rejects, deliveries read 0 (false-negative), not a dup.
cat > "$FAKE/zellij" <<FAKEEOF
#!/usr/bin/env bash
sub="\$*"
case "\$sub" in
  *"list-panes"*)
    # paneId() (pane.cpp:480) walks brace-depth: each TOP-LEVEL {...} is one
    # pane, matched by '"title": "X"' (SPACE after colon), '"is_plugin": false',
    # and '"id": N'. Emit exactly one such pretty-printed object for $RCPT.
    printf '%s\n' '{' '  "id": 1,' '  "is_plugin": false,' '  "title": "$RCPT"' '}'
    ;;
  *"dump-screen"*)
    # detectMode/detectBypass read the footer rows: '-- INSERT --' => INSERT,
    # 'bypass permissions on' => bypass on. Both => delivery is NOT deferred.
    printf '%s\n' "> " "-- INSERT --  bypass permissions on"
    ;;
  *"write-chars"*)
    # THE SINK — one line per delivery write (carries the payload in \$*)
    printf '%s\n' "WRITE \$*" >> "$WRITES"
    ;;
  *) : ;;  # send-keys etc — no-op
esac
exit 0
FAKEEOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

# ── Isolated broker (NOT nohup/setsid/disown) ─────────────────────────────
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

# Drive a broker tick (RPC activity advances the loop — project_broker_tick_rpc_driven)
tick() { "$BUS" state >/dev/null 2>&1; }

# ── The repro ─────────────────────────────────────────────────────────────
PAYLOAD="dup-probe-$(printf '%04d' "$$")"

echo "1. push one record to $RCPT (TTY path -> write-chars #1)"
"$BUS" msg mail "$RCPT" "$PAYLOAD" >/dev/null
tick; sleep 0.1; tick      # let dispatchAgentInbox push it once

echo "2. let the no-ack retry come DUE, then deliver the ack in the SAME window"
sleep 0.1                  # > ack-timeout(50ms): scanRetries now sees retry due
# Append the recipient's UserPromptSubmit ack so the tick has BOTH a due-retry
# AND a pending ack for the in-flight record (the Q2 race). The ORDER the tick
# processes them (scanRetries vs scanEvents) decides dup-vs-clean.
# TODO(finalize, pinned on Q2): elodin confirms whether this intra-tick order
# is fixed/deterministic, or whether forcing needs a delay-injection point he
# adds in the ack/cursor path. For now drive one tick and observe.
TS="$(date -u +%FT%T.%3NZ)"
printf '%s\n' "{\"ts\":\"$TS\",\"session\":\"t\",\"agent\":\"$RCPT\",\"pane\":\"1\",\"event\":\"UserPromptSubmit\",\"payload\":{\"prompt\":\"ok\"}}" \
  >> "$CLAUDE_BUS_STATE/events.jsonl"
tick; sleep 0.1; tick

echo "3. ASSERT exactly-once: the payload was written to the pane exactly 1x"
writes=$(grep -c "$PAYLOAD" "$WRITES" || true)
ck "${writes:-0}" "1" "record delivered to $RCPT EXACTLY once (got $writes write-chars; >=2 == the dup bug)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mDUP-DELIVERY REGRESSION PASSED (exactly-once held)\033[0m"
else
  echo -e "\033[1mDUP-DELIVERY REGRESSION FAILED (duplicate delivered)\033[0m"
  echo "pane-writes.log:"; cat "$WRITES"
  echo "broker.out:"; tail -30 "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
