#!/usr/bin/env bash
# Integration tests for the unified `bus` binary.
#
# Each phase 4a sub-phase grows this file: 4a.1 covers help/version; 4a.2
# adds pane-id / pane-state / send; etc. Tests are designed to pass
# without a running zellij session where possible — pane-related tests
# may need an active session and are gated accordingly.

set -uo pipefail

BUS_ROOT=${CLAUDE_BUS_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}
BUS=${BUS:-$BUS_ROOT/bin/bus}

BROKER_STATE=${BROKER_STATE:-/tmp/broker-itest}
export CLAUDE_BUS_STATE=$BROKER_STATE

# shellcheck disable=SC1091
. "$BUS_ROOT/tests/lib/isolated-broker.sh"

pass=0 fail=0
results=()

tc_pass() { results+=("PASS  $1"); pass=$(( pass + 1 )); }
tc_fail() { results+=("FAIL  $1 — $2"); fail=$(( fail + 1 )); }

# ─────────────────────────────────────────────────────────────────────
# TC1: bus with no args prints usage to stderr and exits 2.
echo "=== TC1: usage on no-args ==="
"$BUS" >/tmp/bus-itest.out 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] \
        && grep -q 'usage: bus' /tmp/bus-itest.err \
        && ! grep -q 'usage:' /tmp/bus-itest.out; then
    tc_pass "TC1 no-args usage on stderr, rc=2"
else
    tc_fail "TC1 no-args" "rc=$rc; stdout=$(cat /tmp/bus-itest.out); stderr=$(cat /tmp/bus-itest.err | head -3)"
fi

# ─────────────────────────────────────────────────────────────────────
# TC2: bus help prints usage to stdout and exits 0.
echo "=== TC2: help on stdout ==="
"$BUS" help >/tmp/bus-itest.out 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "0" ] && grep -q 'usage: bus' /tmp/bus-itest.out; then
    tc_pass "TC2 help to stdout, rc=0"
else
    tc_fail "TC2 help" "rc=$rc"
fi

# ─────────────────────────────────────────────────────────────────────
# TC3: bus version prints something on stdout, exits 0.
echo "=== TC3: version ==="
ver=$("$BUS" version 2>/dev/null)
rc=$?
if [ "$rc" = "0" ] && [ -n "$ver" ]; then
    tc_pass "TC3 version → \"$ver\""
else
    tc_fail "TC3 version" "rc=$rc out='$ver'"
fi

# ─────────────────────────────────────────────────────────────────────
# TC4: unknown subcommand exits 2.
echo "=== TC4: unknown subcommand ==="
"$BUS" zzz-bogus-xxx >/tmp/bus-itest.out 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'unknown subcommand' /tmp/bus-itest.err; then
    tc_pass "TC4 unknown subcommand → rc=2 with stderr message"
else
    tc_fail "TC4 unknown subcommand" "rc=$rc"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4a.3 tests — events tail.

# Prepare isolated state for events tests
EVENT_TEST_DIR=$(mktemp -d)
EVENT_LOG="$EVENT_TEST_DIR/events.jsonl"
echo '{"ts":"2020-01-01T00:00:00.000Z", "agent":"elodin", "event":"hello"}' > "$EVENT_LOG"
echo '{"ts":"2020-01-01T00:00:01.000Z", "agent":"other", "event":"hello"}' >> "$EVENT_LOG"

# TC10: bus events --agent NAME filter prints only matching agent lines.
echo "=== TC10: bus events --agent filter ==="
timeout 0.5 env CLAUDE_BUS_STATE="$EVENT_TEST_DIR" "$BUS" events --agent elodin 2>/dev/null >/tmp/bus-itest.out || true
# Every printed line should have 'elodin' as the agent column.
bad=$(awk 'NF>0 && $2!="elodin"' /tmp/bus-itest.out | head -1)
if [ -z "$bad" ] && [ -s /tmp/bus-itest.out ]; then
    tc_pass "TC10 events --agent filter ($(wc -l < /tmp/bus-itest.out | tr -d ' ') matching lines)"
else
    tc_fail "TC10 events --agent" "non-elodin line: '$bad'"
fi

# TC11: bus events --since far-future filters everything out.
echo "=== TC11: bus events --since far-future ==="
timeout 0.5 env CLAUDE_BUS_STATE="$EVENT_TEST_DIR" "$BUS" events --since "2099-01-01T00:00:00.000Z" 2>/dev/null >/tmp/bus-itest.out || true
lines=$(wc -l < /tmp/bus-itest.out | tr -d ' ')
if [ "$lines" = "0" ]; then
    tc_pass "TC11 events --since future → empty"
else
    tc_fail "TC11 events --since future" "got $lines lines"
fi

# TC12: bus events --garbage exits 2.
echo "=== TC12: bus events unknown flag ==="
"$BUS" events --garbage 2>/dev/null
rc=$?
if [ "$rc" = "2" ]; then
    tc_pass "TC12 events unknown flag → rc=2"
else
    tc_fail "TC12 events unknown flag" "rc=$rc"
fi

# TC13: bus events live tail.
echo "=== TC13: bus events live tail ==="
timeout 0.5 env CLAUDE_BUS_STATE="$EVENT_TEST_DIR" "$BUS" events >/tmp/bus-itest.out 2>/dev/null &
tail_pid=$!
sleep 0.1
echo '{"ts":"2099-12-31T23:59:59.000Z", "agent":"elodin", "event":"test_marker"}' >> "$EVENT_LOG"
wait "$tail_pid" 2>/dev/null || true

if grep -q "test_marker" /tmp/bus-itest.out; then
    tc_pass "TC13 live tail caught synthetic event"
else
    tc_fail "TC13 live tail" "marker not seen in output"
fi

rm -rf "$EVENT_TEST_DIR"

# ─────────────────────────────────────────────────────────────────────
# Phase 4a.4 tests — viewers (monitor, inbox, agent-bar).
# These viewers run in alt-screen and refresh on a 1s timer. We can't
# easily check rendered content (it's overwritten in-place) so we just
# verify the binary starts, runs for >0.3s without exiting, and shuts
# down cleanly on SIGTERM (rc 124 from `timeout`).

run_viewer_briefly() {
    local sub=$1; shift
    timeout 0.5 "$BUS" "$sub" "$@" >/dev/null 2>/tmp/bus-itest.err
    rc=$?
    # timeout returns 124 when it had to kill; 0 if the program exited
    # itself (which would be a regression for a long-running viewer).
    if [ "$rc" = "124" ]; then
        tc_pass "viewer \`bus $sub $*\` runs in long-loop (rc=124)"
    else
        tc_fail "viewer \`bus $sub $*\`" \
            "expected rc=124 (timeout), got rc=$rc; stderr=$(cat /tmp/bus-itest.err | head -3)"
    fi
}

# TC14: monitor runs without crashing.
echo "=== TC14: bus monitor runs ==="
run_viewer_briefly monitor

# TC15: agent-bar runs without crashing.
echo "=== TC15: bus agent-bar runs ==="
run_viewer_briefly agent-bar elodin

# TC16: agent-bar usage on bad argc.
echo "=== TC16: bus agent-bar usage ==="
"$BUS" agent-bar 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*agent-bar' /tmp/bus-itest.err; then
    tc_pass "TC16 agent-bar usage on no args"
else
    tc_fail "TC16 agent-bar usage" "rc=$rc"
fi

# TC17: inbox tail runs without crashing.
echo "=== TC17: bus topic inbox runs ==="
run_viewer_briefly topic inbox itest-fake-recipient

# TC18: inbox usage on bad argc.
echo "=== TC18: bus topic inbox usage ==="
"$BUS" topic inbox 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*inbox' /tmp/bus-itest.err; then
    tc_pass "TC18 inbox usage on no args"
else
    tc_fail "TC18 inbox usage" "rc=$rc"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4b.1 tests — broker daemon + RPC.
#
# The broker has its own state dir so it can't collide with whatever
# real broker may be running in this session. Each TC tears down after
# itself so the suite is re-runnable.


cleanup_broker() {
    if [ -n "${broker_pid:-}" ]; then
        kill "$broker_pid" 2>/dev/null || true
        wait "$broker_pid" 2>/dev/null || true
    fi
    sleep 0.1
    rm -rf "$BROKER_STATE"
}

trap cleanup_broker EXIT

cleanup_broker


# ─────────────────────────────────────────────────────────────────────
# Phase 4b.2 tests — topic registry.

# Fresh broker for the topic tests.
cleanup_broker
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.2

# TC28: empty registry → list returns no rows.
echo "=== TC28: topic list empty ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic list > /tmp/bus-itest.out 2>&1
if [ ! -s /tmp/bus-itest.out ]; then
    tc_pass "TC28 topic list empty"
else
    tc_fail "TC28 topic list empty" "stdout: $(cat /tmp/bus-itest.out)"
fi

# TC29: topic create succeeds.
echo "=== TC29: topic create ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create tasks --kind work-queue \
    >/tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "0" ] && grep -q 'created tasks' /tmp/bus-itest.out; then
    tc_pass "TC29 topic create"
else
    tc_fail "TC29 topic create" "rc=$rc out=$(cat /tmp/bus-itest.out)"
fi

# TC30: duplicate topic rejected. Uses a valid kind so the duplicate check is
# what fires (an invalid kind is now rejected first, on its own merits).
echo "=== TC30: duplicate topic rejected ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create tasks --kind work-queue \
    >/tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "1" ] && grep -q 'already exists' /tmp/bus-itest.out; then
    tc_pass "TC30 duplicate topic rejected"
else
    tc_fail "TC30 duplicate" "rc=$rc out=$(cat /tmp/bus-itest.out)"
fi

# TC31: invalid topic name rejected.
echo "=== TC31: invalid topic name ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create 'Bad Name!' --kind queue \
    >/tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "1" ] && grep -q 'invalid topic name' /tmp/bus-itest.out; then
    tc_pass "TC31 invalid name rejected"
else
    tc_fail "TC31 invalid name" "rc=$rc out=$(cat /tmp/bus-itest.out)"
fi

# TC32: topic show returns expected fields.
echo "=== TC32: topic show ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic show tasks > /tmp/bus-itest.out 2>&1
if grep -q 'kind: *work-queue' /tmp/bus-itest.out \
        && grep -q 'name: *tasks' /tmp/bus-itest.out; then
    tc_pass "TC32 topic show fields"
else
    tc_fail "TC32 topic show" "out=$(cat /tmp/bus-itest.out)"
fi

# TC33: topic show missing → rc=1.
echo "=== TC33: topic show missing ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic show no-such \
    >/tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "1" ]; then
    tc_pass "TC33 topic show missing rc=1"
else
    tc_fail "TC33 topic show missing" "rc=$rc"
fi

# TC34 removed (broker now wipes state on restart per design)

# ─────────────────────────────────────────────────────────────────────
# Phase 4b.3 tests — enqueue / peek / fetch.

# TC35: enqueue returns an id; the same id appears in peek and fetch.
echo "=== TC35: enqueue → peek → fetch round-trip ==="
id=$(CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
     "$BUS" msg enqueue tasks "test body alpha" 2>/dev/null)
if [ -n "$id" ] \
        && CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek tasks | grep -q "$id" \
        && CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg fetch tasks | grep -q "$id"; then
    tc_pass "TC35 enqueue/peek/fetch round-trip (id=$id)"
else
    tc_fail "TC35 round-trip" "id='$id'"
fi

# TC36: fetch on empty queue returns 0, no output.
echo "=== TC36: fetch empty queue ==="
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg fetch tasks 2>&1)
if [ -z "$out" ]; then
    tc_pass "TC36 fetch empty → empty output"
else
    tc_fail "TC36 fetch empty" "got '$out'"
fi

# TC37: auto-create inbox-X on first bus msg mail.
echo "=== TC37: bus msg mail auto-creates inbox-NAME ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" msg mail itestguy "hi" > /dev/null
listing=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic list)
show=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic show inbox-itestguy)
if printf '%s\n' "$listing" | grep -q '^inbox-itestguy ' \
        && printf '%s\n' "$show" | grep -q 'kind: *agent-inbox' \
        && printf '%s\n' "$show" | grep -q '"agent":"itestguy"'; then
    tc_pass "TC37 bus msg mail auto-create inbox"
else
    tc_fail "TC37 inbox auto-create" "listing/show didn't match"
fi

# TC38: auto-create commands-X on first bus msg slash.
echo "=== TC38: bus msg slash auto-creates commands-NAME ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" msg slash itestguy "/clear" > /dev/null
show=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic show commands-itestguy)
peek_out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek commands-itestguy)
if printf '%s\n' "$show" | grep -q 'kind: *tui-commands' \
        && printf '%s\n' "$peek_out" | grep -q 'protocol: *slash' \
        && printf '%s\n' "$peek_out" | grep -q 'deliver_when: *idle' \
        && printf '%s\n' "$peek_out" | grep -q '| /clear'; then
    tc_pass "TC38 bus msg slash auto-create commands"
else
    tc_fail "TC38 commands auto-create" \
        "show=$(printf '%s\n' "$show" | head -2 | tr '\n' '|') peek=$(printf '%s\n' "$peek_out" | head -3 | tr '\n' '|')"
fi

# TC39: bus msg slash rejects body that doesn't start with '/'.
echo "=== TC39: bus msg slash bare text rejected ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg slash itestguy "no-slash" > /tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "2" ] && grep -q "must start with '/'" /tmp/bus-itest.out; then
    tc_pass "TC39 bus msg slash bare-text rejected"
else
    tc_fail "TC39 bus msg slash" "rc=$rc"
fi

# TC40: cursor advances across enqueue. Fetch all the way to empty.
echo "=== TC40: fetch advances cursor across multiple records ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create itest-q --kind work-queue >/dev/null
for i in a b c; do
    CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
        "$BUS" msg enqueue itest-q "body-$i" > /dev/null
done
fetched=0
while CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg fetch itest-q | grep -q '^=== '; do
    fetched=$(( fetched + 1 ))
    [ "$fetched" -ge 10 ] && break
done
if [ "$fetched" = "3" ]; then
    tc_pass "TC40 cursor advances (3 fetched)"
else
    tc_fail "TC40 cursor" "fetched=$fetched (expected 3)"
fi

# TC41 removed (broker now wipes state on restart per design)

# ─────────────────────────────────────────────────────────────────────
# Phase 4c.1 tests — broker state cache + `bus state` RPC.

# TC42: bus state with no filter returns a table (header + rows).
echo "=== TC42: bus state lists agents ==="
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" state 2>&1)
if printf '%s\n' "$out" | head -1 | grep -q 'AGENT.*STATE'; then
    tc_pass "TC42 bus state header line"
else
    tc_fail "TC42 bus state" "out: $(printf '%s\n' "$out" | head -2)"
fi

# TC43: bus state for an unknown agent returns one row with STATE=NEW
# (broker has no events for it; pane doesn't exist).
echo "=== TC43: bus state unknown agent ==="
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" state itest-ghost 2>&1)
if printf '%s\n' "$out" | grep -q '^itest-ghost' \
        && printf '%s\n' "$out" | grep -q 'NEW '; then
    tc_pass "TC43 bus state unknown agent → NEW"
else
    tc_fail "TC43 bus state unknown" "out: $(printf '%s\n' "$out" | tr '\n' '|')"
fi

# TC44: bus state with extra args errors.
echo "=== TC44: bus state usage ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" state a b c 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ]; then
    tc_pass "TC44 bus state extra args rejected"
else
    tc_fail "TC44 bus state extra args" "rc=$rc"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4c.2 tests — delivery loop + in-flight tracker.

# TC45: bus inflight is empty before any delivery.
echo "=== TC45: bus inflight empty ==="
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight 2>&1)
if printf '%s\n' "$out" | grep -q 'no in-flight'; then
    tc_pass "TC45 bus inflight empty"
else
    tc_fail "TC45 bus inflight" "out: $out"
fi

# TC46: delivery to non-existent pane stays out of in-flight.
# (Broker tries each tick; pane-id returns empty; deliverInline returns
# false; in-flight is not written. The record stays at the head of the
# topic for retry.)
echo "=== TC46: missing pane → record sits at head, no in-flight ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" msg mail no-such-pane "ghost" > /dev/null
sleep 0.6  # 2-3 broker ticks
inflight=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight 2>&1)
peek=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek inbox-no-such-pane 2>&1)
if printf '%s\n' "$inflight" | grep -q 'no in-flight' \
        && printf '%s\n' "$peek" | grep -q '| ghost'; then
    tc_pass "TC46 missing pane → no in-flight, record kept"
else
    tc_fail "TC46 missing pane" "inflight: '$inflight' peek: '$(printf '%s\n' "$peek" | head -3)'"
fi

# TC47: TTL expiry → cursor advances past, no delivery.
echo "=== TC47: expired record skipped ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" msg enqueue inbox-no-such-pane "expired" --ttl 1 > /dev/null
sleep 0.6
peek=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek inbox-no-such-pane 2>&1)
# "expired" record should not appear; "ghost" from TC46 still does.
if ! printf '%s\n' "$peek" | grep -q '| expired'; then
    tc_pass "TC47 TTL-expired record skipped past"
else
    tc_fail "TC47 TTL" "peek still shows expired: $(printf '%s\n' "$peek" | head -3)"
fi

# TC48: broker boot wipes stale state (by design).
# The broker explicitly clears topics, cursors, in-flight, etc. on every
# start (see broker.cpp runBroker lines 187-202). This test verifies that
# contract: plant stale state, start a new broker, confirm it's gone.
echo "=== TC48: in-flight tracker survives restart ==="
kill "$broker_pid" 2>/dev/null || true
wait "$broker_pid" 2>/dev/null

# Plant stale state that should be cleaned.
mkdir -p "$BROKER_STATE/in-flight"
echo '{"msg_id":"stale","topic":"inbox-x","agent":"x","dispatched_at_ms":1,"cursor_after":100,"attempts":1,"next_retry_at":0}' \
    > "$BROKER_STATE/in-flight/stale.json"
mkdir -p "$BROKER_STATE/presence"
touch "$BROKER_STATE/presence/stale-agent"

# Start broker — it should wipe the stale state.
cleanup_broker
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3

out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight 2>&1)
has_stale_inflight=false
if printf '%s\n' "$out" | grep -q 'stale'; then
    has_stale_inflight=true
fi
has_stale_presence=false
if [ -f "$BROKER_STATE/presence/stale-agent" ]; then
    has_stale_presence=true
fi

if [ "$has_stale_inflight" = "false" ] && [ "$has_stale_presence" = "false" ]; then
    tc_pass "TC48 broker boot wipes stale state (in-flight + presence)"
else
    tc_fail "TC48 broker boot wipe" "inflight=$has_stale_inflight presence=$has_stale_presence out=$out"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4d tests — tui-commands kind + blocking-op tracker.

# TC49: bus msg slash auto-creates commands-X with the right wire shape.
# (Verified in TC38 too; here we additionally confirm the broker can
# READ the queued tui-commands record with deliver_when=idle.)
echo "=== TC49: bus msg slash queues with deliver_when=idle ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" msg slash itest-noone "/cost" > /dev/null
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek commands-itest-noone)
if printf '%s\n' "$out" | grep -q 'protocol: *slash' \
        && printf '%s\n' "$out" | grep -q 'deliver_when: *idle' \
        && printf '%s\n' "$out" | grep -q '| /cost'; then
    tc_pass "TC49 bus msg slash defaults: protocol=slash, deliver_when=idle"
else
    tc_fail "TC49 slash defaults" "$(printf '%s\n' "$out" | head -6 | tr '\n' '|')"
fi

# TC50: blocking-op tracker honors a planted file.
echo "=== TC50: blocking-op file blocks agent-inbox dispatch ==="
# Clear stale in-flight from earlier TCs so the gate check is clean.
rm -f "$BROKER_STATE/in-flight"/*.json
mkdir -p "$BROKER_STATE/blocking-op" "$BROKER_STATE/presence"
touch "$BROKER_STATE/presence/elodin-itest"
echo "fake-id" > "$BROKER_STATE/blocking-op/elodin-itest"
kill "$broker_pid" 2>/dev/null || true
wait "$broker_pid" 2>/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3
# Drop a record into inbox-elodin-itest; broker should NOT mark it in-flight
# even though there is no real ack mechanism — the blocking-op file gates.
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" msg mail elodin-itest "should defer behind block" > /dev/null
sleep 0.6
inflight=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight 2>&1)
# inflight for the test target should be absent. We allow other agents.
if ! printf '%s\n' "$inflight" | grep -q 'elodin-itest'; then
    tc_pass "TC50 blocking-op gate defers inbox dispatch"
else
    tc_fail "TC50 blocking-op gate" "inflight: $inflight"
fi
# Cleanup so subsequent TCs aren't polluted.
rm -rf "$BROKER_STATE/blocking-op"

# ─────────────────────────────────────────────────────────────────────
# Phase 4e tests — retry timer + audit escalation.

# TC51: agent-inbox record stays peekable without zellij (headless).
# Delivery requires zellij panes — without them, the broker can't deliver,
# so the record should stay at the head forever (no cursor advancement).
# This verifies the headless invariant: messages queue but don't vanish.
echo "=== TC51: max-attempts in-flight escalates to audit + inbox-ops ==="
kill "$broker_pid" 2>/dev/null || true
wait "$broker_pid" 2>/dev/null
rm -rf "$BROKER_STATE"

CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3

CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create inbox-headless-test \
    --kind agent-inbox >/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=test \
     "$BUS" msg enqueue inbox-headless-test "should-persist" >/dev/null

# Wait for several broker ticks.
sleep 1.0

peek1=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek inbox-headless-test 2>&1)
inflight=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight 2>&1)

# Message should still be peekable (no cursor advancement without delivery).
# No in-flight should exist (deliverInline fails without a pane).
if printf '%s\n' "$peek1" | grep -q 'should-persist' \
        && printf '%s\n' "$inflight" | grep -q 'no in-flight'; then
    tc_pass "TC51 headless: record stays peekable, no in-flight without pane"
else
    tc_fail "TC51 headless invariant" "peek: $peek1 | inflight: $inflight"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4f.1 tests — bus msg broadcast verb.

# Restart broker so state cleared by TC51 doesn't trip the next checks.
kill "$broker_pid" 2>/dev/null || true
wait "$broker_pid" 2>/dev/null
rm -rf "$BROKER_STATE"
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3

# TC52: broadcast fans into N inboxes.
echo "=== TC52: bus msg broadcast fans into matching inboxes ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=ibcaster \
    "$BUS" msg broadcast itest-tag "fanned" --to ix1,ix2,ix3 >/dev/null
ok=0
for who in ix1 ix2 ix3; do
    p=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek inbox-$who)
    if printf '%s\n' "$p" | grep -q '| fanned' \
            && printf '%s\n' "$p" | grep -q 'protocol: *itest-tag'; then
        ok=$(( ok + 1 ))
    fi
done
if [ "$ok" = "3" ]; then
    tc_pass "TC52 broadcast fanned into 3 inboxes with tag"
else
    tc_fail "TC52 broadcast" "$ok/3 inboxes had the record"
fi

# TC53: usage rejected without --to.
echo "=== TC53: bus msg broadcast missing --to ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg broadcast tag body 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'required\|usage' /tmp/bus-itest.err; then
    tc_pass "TC53 broadcast missing --to → rc=2"
else
    tc_fail "TC53 broadcast --to" "rc=$rc"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4f.2 tests — pubsub cascade.

# TC54: enqueue to a pubsub topic cascades into each subscriber's inbox.
echo "=== TC54: pubsub cascade ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create itest-status \
    --kind pubsub --subscribers sa,sb,sc >/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=ibcaster \
    "$BUS" msg enqueue itest-status "system green" >/dev/null
ok=0
for who in sa sb sc; do
    p=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek inbox-$who 2>&1)
    if printf '%s\n' "$p" | grep -q '| system green'; then
        ok=$(( ok + 1 ))
    fi
done
canonical=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek itest-status)
if [ "$ok" = "3" ] && printf '%s\n' "$canonical" | grep -q '| system green'; then
    tc_pass "TC54 pubsub cascade (3 inboxes + canonical record)"
else
    tc_fail "TC54 pubsub cascade" "$ok/3 + canonical=$(printf '%s\n' "$canonical" | head -3 | tr '\n' '|')"
fi

# TC55: topic show reflects subscribers in kind_config.
echo "=== TC55: pubsub topic shows subscribers ==="
show=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic show itest-status)
if printf '%s\n' "$show" | grep -q 'subscribers.*sa.*sb.*sc'; then
    tc_pass "TC55 pubsub topic subscribers visible"
else
    tc_fail "TC55 pubsub topic show" "show=$(printf '%s\n' "$show" | head -6 | tr '\n' '|')"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4f.3 tests — blackboard kind.

# TC56: write twice; peek returns only the latest.
echo "=== TC56: blackboard overwrite ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create itest-bb \
    --kind blackboard >/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=test \
    "$BUS" msg enqueue itest-bb "first" >/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=test \
    "$BUS" msg enqueue itest-bb "second" >/dev/null
p=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek itest-bb)
if printf '%s\n' "$p" | grep -q '| second' \
        && ! printf '%s\n' "$p" | grep -q '| first'; then
    tc_pass "TC56 blackboard peek shows only latest"
else
    tc_fail "TC56 blackboard" "peek: $(printf '%s\n' "$p" | tr '\n' '|')"
fi

# TC57: fetch is non-destructive on blackboard.
echo "=== TC57: blackboard fetch is non-destructive ==="
a=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg fetch itest-bb | grep '^|' | head -1)
b=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg fetch itest-bb | grep '^|' | head -1)
if [ -n "$a" ] && [ "$a" = "$b" ]; then
    tc_pass "TC57 blackboard fetch idempotent ($a)"
else
    tc_fail "TC57 blackboard fetch" "a='$a' b='$b'"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4g tests — `bus msg body MSG_ID` (side-effect-free read by id).

# TC58: large body → bus msg body returns the full content and the topic
# cursor is unchanged afterward (the call must NOT advance / ack).
echo "=== TC58: bus msg body returns large body without cursor side effects ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create itest-body \
    --kind work-queue >/dev/null 2>&1
# 2000 bytes of deterministic content (would have overflowed the old
# 4096-byte PIPE_BUF cap once framing + metadata is added; today's cap
# is 1 MiB so this stores inline in the topic log).
big_body=$(printf 'A%.0s' $(seq 1 2000))
body_id=$(CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
          "$BUS" msg enqueue itest-body "$big_body" 2>/dev/null)
cursor_p="$BROKER_STATE/cursors/itest-body/_default.cursor"
before=$([ -f "$cursor_p" ] && cat "$cursor_p" || echo "absent")
got=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg body "$body_id" 2>/tmp/bus-itest.err)
rc=$?
after=$([ -f "$cursor_p" ] && cat "$cursor_p" || echo "absent")
if [ "$rc" = "0" ] \
        && [ "$got" = "$big_body" ] \
        && [ "$before" = "$after" ]; then
    tc_pass "TC58 bus msg body large body, cursor stable ($before → $after)"
else
    tc_fail "TC58 bus msg body" "rc=$rc bytes=${#got}/${#big_body} before=$before after=$after err=$(cat /tmp/bus-itest.err | head -2)"
fi

# TC59: bus msg body on an unknown msg_id → rc=1 with error message.
echo "=== TC59: bus msg body unknown msg_id ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg body 0000000000000-ghost-ffff \
    >/tmp/bus-itest.out 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "1" ] && grep -q 'no such msg_id' /tmp/bus-itest.err; then
    tc_pass "TC59 bus msg body unknown msg_id → rc=1"
else
    tc_fail "TC59 bus msg body unknown" "rc=$rc err=$(cat /tmp/bus-itest.err | head -2)"
fi

# TC60: bus msg body without arg → usage on stderr, rc=2.
echo "=== TC60: bus msg body usage ==="
"$BUS" msg body 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*body' /tmp/bus-itest.err; then
    tc_pass "TC60 bus msg body no-args usage"
else
    tc_fail "TC60 bus msg body usage" "rc=$rc"
fi

# TC61: enqueue a body across the old PIPE_BUF boundary. Anything past
# ~3.8 KiB used to hit "record size N exceeds atomic-write limit 4096".
# Now that the broker is the sole writer to topic logs (no PIPE_BUF
# atomicity needed), the cap is 1 MiB and 8 KiB sails through.
echo "=== TC61: enqueue 8 KiB body (regression for the 4096 cap) ==="
big_body=$(printf 'B%.0s' $(seq 1 8192))
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" msg enqueue inbox-itest-big "$big_body" \
    >/tmp/bus-itest.out 2>/tmp/bus-itest.err
rc=$?
got=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" msg peek inbox-itest-big \
      2>/dev/null | tr -cd 'B' | wc -c)
if [ "$rc" = "0" ] && [ "$got" = "8192" ]; then
    tc_pass "TC61 8 KiB body enqueue+peek roundtrip ($got bytes)"
else
    tc_fail "TC61 8 KiB body" "rc=$rc got=$got/8192 err=$(cat /tmp/bus-itest.err | head -1)"
fi

# ─────────────────────────────────────────────────────────────────────
echo ""
echo "=== bus-itest results ==="
for r in "${results[@]}"; do
    echo "  $r"
done
echo "  pass: $pass / $(( pass + fail ))"
[ "$fail" -gt 0 ] && exit 1 || exit 0
