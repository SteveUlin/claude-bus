#!/usr/bin/env bash
# dup-delivery-itest.sh — DETERMINISTIC regression for the DUPLICATE-DELIVERY
# broker bug (sulin-prioritized): a record delivered to a recipient TWICE
# (report: an auri->comms message arrived byte-identical twice). bast owns this
# failing regression; elodin owns the fix (verification-parallelism).
#
# ── ROOT CAUSE (elodin, confirmed + simplified 2026-06-01) ────────────────
# It is STRUCTURAL, not an ack-timing race. comms is a hardcoded TTY opt-out
# (tty_policy.h:26), so dispatchAgentInbox takes the TTY PUSH path. There
# (delivery.cpp:643) a record is marked in-flight ONLY AFTER deliverInline
# SUCCEEDS:  `if (!deliverInline(...)) return;`  — so EVERY in-flight record
# has ALREADY landed on the pane. Then scanRetries (861) calls deliverInline
# AGAIN on that same in-flight record -> re-pushes the identical bytes ->
# guaranteed duplicate (and it appends into the pane's input buffer, so it can
# garble / double-submit). No ack is needed to trigger it: in-flight ⟹
# already-delivered, so ANY retry re-push is a dup. Current binary re-pushes
# up to kMaxAttempts(3) then escalates -> 3 identical deliveries.
# (The off-TTY path already does the right thing: it acks BY ID and escalates,
# never TTY-re-dispatches — delivery.cpp:434-454.)
#
# ── THE REGRESSION ────────────────────────────────────────────────────────
# Mail an idle TTY agent, never ack, drive ticks past kMaxAttempts*ackTimeout.
# Lock elodin's invariant (two clauses):
#   (1) a successfully-delivered agent-inbox record is pushed to the pane
#       EXACTLY ONCE  -> assert write-chars count for the payload == 1; and
#   (2) absence of an ack triggers ESCALATION (inbox-ops/audit), NOT
#       re-delivery -> assert an escalation record appears.
# Pre-fix: the record is pushed 3x (FAIL clause 1) AND escalation appears
# (clause 2 already holds). Post-fix (scanRetries escalates on the ack-DEADLINE
# instead of re-delivering): pushed 1x + escalation -> both pass.
#
# SINK: a fake zellij logs every write-chars (the literal pane delivery);
# count occurrences of the unique payload. >=2 == the dup. (elodin-confirmed.)
#
# Gotchas baked in (project_broker_itest_gotchas + project_fake_zellij_
# delivery_harness): assert on the fake sink, never a racy client read;
# `grep -c || true`; wait only on named pids.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/dup-delivery-itest.XXXXXX)"
export CLAUDE_BUS_ACK_TIMEOUT_MS=200    # retry/escalation deadline comes fast

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

echo "1. mail an idle TTY agent — delivered once, marked in-flight"
"$BUS" msg mail "$RCPT" "$PAYLOAD" >/dev/null
tick; sleep 0.2     # dispatch -> push #1, in-flight, next_retry_at set

echo "2. NEVER ack; drive ticks past kMaxAttempts*ackTimeout"
for _ in $(seq 1 12); do tick; sleep 0.2; done   # ~2.4s >> 3*200ms

echo "3. ASSERT exactly-once delivery (a delivered record must not be re-pushed)"
writes=$(grep -c "$PAYLOAD" "$WRITES" || true)
ck "${writes:-0}" "1" "record pushed to $RCPT EXACTLY once (got $writes; >=2 == the dup bug)"

# The mail payload is multiline (header\nbody); the fix flattens '\n'→space so
# header + body land on ONE write-chars line + one Enter (a raw \n would submit
# the header alone and fragment the rest — the comms draft-clobber bug). Assert
# the payload's write line ALSO carries the header (flattened together, not
# \n-split onto separate lines).
flat=$(grep "$PAYLOAD" "$WRITES" | grep -c "bus msg mail from" || true)
ck "$([ "${flat:-0}" -ge 1 ] && echo flat)" "flat" \
   "multiline TTY push flattened to one line (header+body together; count=$flat)"

echo "4. ASSERT the no-ack response is ESCALATION, not re-delivery"
esc_audit=$("$BUS" msg peek audit --limit 100 2>/dev/null | grep -c "delivery exhausted.*agent=$RCPT" || true)
esc_ops=$("$BUS" msg peek inbox-ops --limit 100 2>/dev/null | grep -c "delivery to $RCPT exhausted" || true)
ck "$([ "${esc_audit:-0}" -ge 1 ] || [ "${esc_ops:-0}" -ge 1 ] && echo escalated)" "escalated" \
   "escalation recorded after the ack deadline (audit=$esc_audit ops=$esc_ops)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mDUP-DELIVERY REGRESSION PASSED (exactly-once + escalate-not-redeliver)\033[0m"
else
  echo -e "\033[1mDUP-DELIVERY REGRESSION FAILED — duplicate delivered (expected on the pre-fix binary)\033[0m"
  echo "pane-writes.log (count=$writes):"; cat "$WRITES"
fi
exit "$fail"
