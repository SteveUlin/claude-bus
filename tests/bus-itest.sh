#!/usr/bin/env bash
# Integration tests for the unified `bus` binary.
#
# Each phase 4a sub-phase grows this file: 4a.1 covers help/version; 4a.2
# adds pane-id / pane-state / send; etc. Tests are designed to pass
# without a running zellij session where possible — pane-related tests
# may need an active session and are gated accordingly.

set -uo pipefail

BUS=${BUS:-/home/sulin/claude-bus/bin/bus}

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
# Phase 4a.2 tests — pane-id / pane-state / send.
#
# These require a live zellij session with at least elodin's pane
# visible. Skip cleanly if not.

probe_pane=$("$BUS" pane-id elodin 2>/dev/null || true)
if [ -z "$probe_pane" ]; then
    echo "(no live zellij session — skipping pane-id / pane-state TCs)"
else
    # TC5: bus pane-id returns terminal_N format.
    echo "=== TC5: pane-id returns terminal_N ==="
    new_out=$("$BUS" pane-id elodin 2>/dev/null)
    if printf '%s' "$new_out" | grep -qE '^terminal_[0-9]+$'; then
        tc_pass "TC5 pane-id format ($new_out)"
    else
        tc_fail "TC5 pane-id format" "got '$new_out'"
    fi

    # TC6: bus pane-id missing → rc=1.
    echo "=== TC6: pane-id missing → rc=1 ==="
    "$BUS" pane-id zzz-no-such-pane >/dev/null 2>&1
    rc=$?
    if [ "$rc" = "1" ]; then
        tc_pass "TC6 pane-id missing rc=1"
    else
        tc_fail "TC6 pane-id missing" "rc=$rc"
    fi

    # TC7: bus pane-state on a claude pane returns the expected keys.
    echo "=== TC7: pane-state contains expected keys ==="
    new_out=$("$BUS" pane-state elodin 2>/dev/null)
    needed="pane: mode: buffer: suggestion: bypass_perms:"
    miss=""
    for k in $needed; do
        echo "$new_out" | grep -q "^$k" || miss="$miss $k"
    done
    if [ -z "$miss" ]; then
        tc_pass "TC7 pane-state keys present"
    else
        tc_fail "TC7 pane-state keys missing" "$miss"
    fi

    # TC8: pane-state for a non-claude pane (monitor) — should report
    # mode=unknown (no `-- INSERT --` indicator).
    echo "=== TC8: pane-state non-claude reports mode=unknown ==="
    new_out=$("$BUS" pane-state monitor 2>/dev/null)
    mode=$(printf '%s\n' "$new_out" | awk -F: '$1=="mode"{print $2}' | xargs)
    if [ "$mode" = "unknown" ]; then
        tc_pass "TC8 pane-state non-claude mode=unknown"
    else
        tc_fail "TC8 pane-state non-claude" "got mode='$mode'"
    fi
fi

# TC9: bus send usage on bad argc.
echo "=== TC9: bus send usage ==="
"$BUS" send 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*send' /tmp/bus-itest.err; then
    tc_pass "TC9 bus send usage on no args"
else
    tc_fail "TC9 bus send usage" "rc=$rc"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4a.3 tests — events tail.

# TC10: bus events --agent NAME filter prints only matching agent lines.
echo "=== TC10: bus events --agent filter ==="
timeout 0.5 "$BUS" events --agent elodin 2>/dev/null >/tmp/bus-itest.out || true
# Every printed line should have 'elodin' as the agent column.
bad=$(awk 'NF>0 && $2!="elodin"' /tmp/bus-itest.out | head -1)
if [ -z "$bad" ] && [ -s /tmp/bus-itest.out ]; then
    tc_pass "TC10 events --agent filter ($(wc -l < /tmp/bus-itest.out | tr -d ' ') matching lines)"
else
    tc_fail "TC10 events --agent" "non-elodin line: '$bad'"
fi

# TC11: bus events --since far-future filters everything out.
echo "=== TC11: bus events --since far-future ==="
timeout 0.5 "$BUS" events --since "2099-01-01T00:00:00.000Z" 2>/dev/null >/tmp/bus-itest.out || true
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

# TC13: live tail — append a synthetic event, see it appear within ~1s.
echo "=== TC13: bus events live tail ==="
marker="bus-itest-marker-$(date +%s)-$RANDOM"
timeout 1.5 "$BUS" events --agent "$marker" >/tmp/bus-itest.out 2>/dev/null &
bus_pid=$!
sleep 0.3
# Append a synthetic JSONL line that mimics log-event.sh's shape.
printf '{"ts":"%s","session":"itest","agent":"%s","pane":"x","event":"Synthetic","payload":{}}\n' \
    "$(date -u +%FT%T.%3NZ)" "$marker" \
    >> /tmp/claude-bus/events.jsonl
wait "$bus_pid" 2>/dev/null || true
if grep -q "$marker" /tmp/bus-itest.out; then
    tc_pass "TC13 live tail caught synthetic event"
else
    tc_fail "TC13 live tail" "marker not seen in output"
fi

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
echo "=== TC17: bus inbox runs ==="
run_viewer_briefly inbox itest-fake-recipient

# TC18: inbox usage on bad argc.
echo "=== TC18: bus inbox usage ==="
"$BUS" inbox 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*inbox' /tmp/bus-itest.err; then
    tc_pass "TC18 inbox usage on no args"
else
    tc_fail "TC18 inbox usage" "rc=$rc"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4a.5 tests — spawn, up.

# TC19 (was: bus up idempotent against bin/watcher) — retired in
# phase 4g. The broker singleton-guard check (TC22-TC27) covers the
# same liveness story now.

# TC20: bus up with extra args errors.
echo "=== TC20: bus up rejects extra args ==="
"$BUS" up garbage 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*up' /tmp/bus-itest.err; then
    tc_pass "TC20 bus up rejects extra args"
else
    tc_fail "TC20 bus up extra args" "rc=$rc"
fi

# TC21: bus spawn usage on no args.
echo "=== TC21: bus spawn usage ==="
"$BUS" spawn 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*spawn' /tmp/bus-itest.err; then
    tc_pass "TC21 bus spawn usage on no args"
else
    tc_fail "TC21 bus spawn usage" "rc=$rc"
fi

# (We don't run a real `bus spawn NAME` in the test suite — it creates
# a zellij tab and would steal focus from sulin's active session. The
# `bus up` idempotency check is enough to prove the daemon-spawn path
# works end-to-end via the same selfDir() + fork+exec path.)

# ─────────────────────────────────────────────────────────────────────
# Phase 4b.1 tests — broker daemon + RPC.
#
# The broker has its own state dir so it can't collide with whatever
# real broker may be running in this session. Each TC tears down after
# itself so the suite is re-runnable.

BROKER_STATE=${BROKER_STATE:-/tmp/broker-itest}
export CLAUDE_BUS_STATE=$BROKER_STATE

cleanup_broker() {
    CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null 2>&1 || true
    sleep 0.1
    rm -rf "$BROKER_STATE"
}

trap cleanup_broker EXIT

cleanup_broker

# TC22: broker not running → status returns 1 with "not running".
echo "=== TC22: broker status when not running ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker status > /tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "1" ] && grep -q 'not running' /tmp/bus-itest.out; then
    tc_pass "TC22 broker status (not running)"
else
    tc_fail "TC22 broker status" "rc=$rc out=$(cat /tmp/bus-itest.out)"
fi

# TC23: start broker, status becomes alive.
echo "=== TC23: broker run + status ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker status > /tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "0" ] && grep -q 'alive' /tmp/bus-itest.out; then
    tc_pass "TC23 broker status (alive)"
else
    tc_fail "TC23 broker status (alive)" "rc=$rc out=$(cat /tmp/bus-itest.out)"
fi

# TC24: second broker fails with already-running error.
echo "=== TC24: second broker rejected ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "1" ] && grep -q 'already running' /tmp/bus-itest.out; then
    tc_pass "TC24 second broker rejected"
else
    tc_fail "TC24 second broker" "rc=$rc out=$(cat /tmp/bus-itest.out)"
fi

# TC25: ping via RPC socket. We construct a minimal JSON-RPC client
# in bash + socat to verify the wire protocol independently of the
# bus client library.
echo "=== TC25: ping over raw socket ==="
if command -v socat >/dev/null 2>&1; then
    reply=$(printf '%s\n' '{"op":"ping"}' \
        | timeout 1 socat - UNIX-CONNECT:"$BROKER_STATE/broker.sock" 2>/dev/null)
    if [ "$reply" = '{"ok":true}' ]; then
        tc_pass "TC25 ping → $reply"
    else
        tc_fail "TC25 ping" "got '$reply'"
    fi
else
    echo "(socat not available — skipping raw-socket test)"
fi

# TC26: stop broker, status returns to not running.
echo "=== TC26: broker stop ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop > /tmp/bus-itest.out 2>&1
rc_stop=$?
wait "$broker_pid" 2>/dev/null
broker_rc=$?
sleep 0.1
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker status > /dev/null 2>&1
rc_status=$?
if [ "$rc_stop" = "0" ] && [ "$broker_rc" = "0" ] && [ "$rc_status" = "1" ]; then
    tc_pass "TC26 broker stop (stop=0 broker=0 status=1)"
else
    tc_fail "TC26 broker stop" "stop=$rc_stop broker=$broker_rc status=$rc_status"
fi

# TC27: socket and pid file cleaned up after stop.
echo "=== TC27: broker cleans up files ==="
if [ ! -e "$BROKER_STATE/broker.sock" ] && [ ! -e "$BROKER_STATE/broker.pid" ]; then
    tc_pass "TC27 socket + pid file cleaned up"
else
    tc_fail "TC27 cleanup" \
        "sock=$([ -e $BROKER_STATE/broker.sock ] && echo present || echo absent), pid=$([ -e $BROKER_STATE/broker.pid ] && echo present || echo absent)"
fi

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

# TC30: duplicate topic rejected.
echo "=== TC30: duplicate topic rejected ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create tasks --kind whatever \
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

# TC34: registry survives broker restart.
echo "=== TC34: topic registry persistence ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null
wait "$broker_pid" 2>/dev/null
[ -s "$BROKER_STATE/topics.json" ]; persisted=$?
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.2
restored=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic list 2>&1)
if [ "$persisted" = "0" ] && printf '%s\n' "$restored" | grep -q 'tasks'; then
    tc_pass "TC34 registry persistence (topics.json present, tasks restored)"
else
    tc_fail "TC34 persistence" "persisted=$persisted restored='$restored'"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4b.3 tests — enqueue / peek / fetch.

# TC35: enqueue returns an id; the same id appears in peek and fetch.
echo "=== TC35: enqueue → peek → fetch round-trip ==="
id=$(CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
     "$BUS" enqueue tasks "test body alpha" 2>/dev/null)
if [ -n "$id" ] \
        && CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek tasks | grep -q "$id" \
        && CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" fetch tasks | grep -q "$id"; then
    tc_pass "TC35 enqueue/peek/fetch round-trip (id=$id)"
else
    tc_fail "TC35 round-trip" "id='$id'"
fi

# TC36: fetch on empty queue returns 0, no output.
echo "=== TC36: fetch empty queue ==="
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" fetch tasks 2>&1)
if [ -z "$out" ]; then
    tc_pass "TC36 fetch empty → empty output"
else
    tc_fail "TC36 fetch empty" "got '$out'"
fi

# TC37: auto-create inbox-X on first bus mail.
echo "=== TC37: bus mail auto-creates inbox-NAME ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" mail itestguy "hi" > /dev/null
listing=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic list)
show=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic show inbox-itestguy)
if printf '%s\n' "$listing" | grep -q '^inbox-itestguy ' \
        && printf '%s\n' "$show" | grep -q 'kind: *agent-inbox' \
        && printf '%s\n' "$show" | grep -q '"agent":"itestguy"'; then
    tc_pass "TC37 bus mail auto-create inbox"
else
    tc_fail "TC37 inbox auto-create" "listing/show didn't match"
fi

# TC38: auto-create commands-X on first bus slash.
echo "=== TC38: bus slash auto-creates commands-NAME ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" slash itestguy "/clear" > /dev/null
show=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic show commands-itestguy)
peek_out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek commands-itestguy)
if printf '%s\n' "$show" | grep -q 'kind: *tui-commands' \
        && printf '%s\n' "$peek_out" | grep -q 'protocol: *slash' \
        && printf '%s\n' "$peek_out" | grep -q 'deliver_when: *idle' \
        && printf '%s\n' "$peek_out" | grep -q '| /clear'; then
    tc_pass "TC38 bus slash auto-create commands"
else
    tc_fail "TC38 commands auto-create" \
        "show=$(printf '%s\n' "$show" | head -2 | tr '\n' '|') peek=$(printf '%s\n' "$peek_out" | head -3 | tr '\n' '|')"
fi

# TC39: bus slash rejects body that doesn't start with '/'.
echo "=== TC39: bus slash bare text rejected ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" slash itestguy "no-slash" > /tmp/bus-itest.out 2>&1
rc=$?
if [ "$rc" = "2" ] && grep -q "must start with '/'" /tmp/bus-itest.out; then
    tc_pass "TC39 bus slash bare-text rejected"
else
    tc_fail "TC39 bus slash" "rc=$rc"
fi

# TC40: cursor advances across enqueue. Fetch all the way to empty.
echo "=== TC40: fetch advances cursor across multiple records ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create itest-q --kind work-queue >/dev/null
for i in a b c; do
    CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
        "$BUS" enqueue itest-q "body-$i" > /dev/null
done
fetched=0
while CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" fetch itest-q | grep -q '^=== '; do
    fetched=$(( fetched + 1 ))
    [ "$fetched" -ge 10 ] && break
done
if [ "$fetched" = "3" ]; then
    tc_pass "TC40 cursor advances (3 fetched)"
else
    tc_fail "TC40 cursor" "fetched=$fetched (expected 3)"
fi

# TC41: topic-log file persists records across broker restart.
echo "=== TC41: topic data persists across restart ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" enqueue tasks "survive-restart" > /dev/null
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null
wait "$broker_pid" 2>/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.2
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek tasks)
if printf '%s\n' "$out" | grep -q 'survive-restart'; then
    tc_pass "TC41 topic records persist across restart"
else
    tc_fail "TC41 persistence" "peek: $(printf '%s\n' "$out" | head -4)"
fi

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
    "$BUS" mail no-such-pane "ghost" > /dev/null
sleep 0.6  # 2-3 broker ticks
inflight=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight 2>&1)
peek=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek inbox-no-such-pane 2>&1)
if printf '%s\n' "$inflight" | grep -q 'no in-flight' \
        && printf '%s\n' "$peek" | grep -q '| ghost'; then
    tc_pass "TC46 missing pane → no in-flight, record kept"
else
    tc_fail "TC46 missing pane" "inflight: '$inflight' peek: '$(printf '%s\n' "$peek" | head -3)'"
fi

# TC47: TTL expiry → cursor advances past, no delivery.
echo "=== TC47: expired record skipped ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" mail no-such-pane "expired" --ttl 1 > /dev/null
sleep 0.6
peek=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek inbox-no-such-pane 2>&1)
# "expired" record should not appear; "ghost" from TC46 still does.
if ! printf '%s\n' "$peek" | grep -q '| expired'; then
    tc_pass "TC47 TTL-expired record skipped past"
else
    tc_fail "TC47 TTL" "peek still shows expired: $(printf '%s\n' "$peek" | head -3)"
fi

# TC48: in-flight survives broker restart.
# We use a non-existent pane so the inflight file stays put across
# the restart (real delivery would have acked by now).
echo "=== TC48: in-flight tracker survives restart ==="
# Manually plant an in-flight file mirroring what the broker would write.
mkdir -p "$BROKER_STATE/in-flight"
cat > "$BROKER_STATE/in-flight/test-id.json" <<EOF
{"msg_id":"test-id","topic":"inbox-no-such-pane","agent":"no-such-pane","dispatched_at_ms":123,"cursor_after":456}
EOF
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null
wait "$broker_pid" 2>/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight 2>&1)
if printf '%s\n' "$out" | grep -q 'test-id'; then
    tc_pass "TC48 in-flight survives restart"
else
    tc_fail "TC48 in-flight restart" "out: $out"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4d tests — tui-commands kind + blocking-op tracker.

# TC49: bus slash auto-creates commands-X with the right wire shape.
# (Verified in TC38 too; here we additionally confirm the broker can
# READ the queued tui-commands record with deliver_when=idle.)
echo "=== TC49: bus slash queues with deliver_when=idle ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" slash itest-noone "/cost" > /dev/null
out=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek commands-itest-noone)
if printf '%s\n' "$out" | grep -q 'protocol: *slash' \
        && printf '%s\n' "$out" | grep -q 'deliver_when: *idle' \
        && printf '%s\n' "$out" | grep -q '| /cost'; then
    tc_pass "TC49 bus slash defaults: protocol=slash, deliver_when=idle"
else
    tc_fail "TC49 slash defaults" "$(printf '%s\n' "$out" | head -6 | tr '\n' '|')"
fi

# TC50: blocking-op tracker honors a planted file.
echo "=== TC50: blocking-op file blocks agent-inbox dispatch ==="
# Clear stale in-flight from earlier TCs so the gate check is clean.
rm -f "$BROKER_STATE/in-flight"/*.json
mkdir -p "$BROKER_STATE/blocking-op"
echo "fake-id" > "$BROKER_STATE/blocking-op/elodin-itest"
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null
wait "$broker_pid" 2>/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3
# Drop a record into inbox-elodin-itest; broker should NOT mark it in-flight
# even though there is no real ack mechanism — the blocking-op file gates.
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" mail elodin-itest "should defer behind block" > /dev/null
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

# TC50b: pending /clear on commands-X gates inbox-X dispatch.
# Without this gate, `bus slash X /clear` followed by `bus mail X "..."`
# raced: /clear waited on the deliver_when=idle gate while mail (no
# idle gate) dispatched immediately, landing in pre-clear context.
#
# Runs in an isolated state dir + broker so it's not contended by the
# pending /clear records left around by TC38 / TC49 (those never
# dispatch — no real agent — so they sit on commands-itestguy forever
# and the broker logs "defer for itestguy" every tick, making it hard
# to observe a single race-itest tick within a reasonable sleep).
echo "=== TC50b: pending /clear on commands-X gates inbox-X ==="
ISO_STATE=$(mktemp -d -t tc50b-XXXXXX)
mkdir -p "$ISO_STATE/topics"
CLAUDE_BUS_STATE=$ISO_STATE "$BUS" broker run > "$ISO_STATE/broker.log" 2>&1 &
iso_broker_pid=$!
sleep 0.5
CLAUDE_BUS_STATE=$ISO_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" slash race-iso "/clear" > /dev/null
CLAUDE_BUS_STATE=$ISO_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" mail race-iso "should defer behind pending /clear" > /dev/null
# Broker ticks every 250ms; gate fires on each tick that processes the
# inbox topic. Poll up to 5s for the log line.
elapsed=0
while [ $elapsed -lt 5 ]; do
    if grep -q "defer agent-inbox dispatch for race-iso" "$ISO_STATE/broker.log"; then
        break
    fi
    sleep 1
    elapsed=$(( elapsed + 1 ))
done
if grep -q "defer agent-inbox dispatch for race-iso" "$ISO_STATE/broker.log"; then
    tc_pass "TC50b pending /clear gates inbox dispatch"
else
    log_tail=$(tail -5 "$ISO_STATE/broker.log" 2>/dev/null | tr '\n' '|')
    tc_fail "TC50b pending /clear gate" "no defer log after 5s; log tail: [$log_tail]"
fi
# Cleanup.
CLAUDE_BUS_STATE=$ISO_STATE "$BUS" broker stop > /dev/null 2>&1
wait "$iso_broker_pid" 2>/dev/null
rm -rf "$ISO_STATE"

# ─────────────────────────────────────────────────────────────────────
# Phase 4e tests — retry timer + audit escalation.

# TC51: plant an in-flight at max attempts → broker restart escalates
# on the first retry tick.
echo "=== TC51: max-attempts in-flight escalates to audit + inbox-ops ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null 2>&1
wait "$broker_pid" 2>/dev/null
rm -rf "$BROKER_STATE"

# Short ack timeout for testing.
export CLAUDE_BUS_ACK_TIMEOUT_MS=500
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create inbox-itester \
    --kind agent-inbox >/dev/null
ID=$(CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=test \
     "$BUS" enqueue inbox-itester "stuck-marker")
mkdir -p "$BROKER_STATE/in-flight"
NOW=$(date +%s%3N)
cat > "$BROKER_STATE/in-flight/$ID.json" <<EOF
{"msg_id":"$ID","topic":"inbox-itester","agent":"itester","dispatched_at_ms":$NOW,"cursor_after":1000000,"attempts":3,"next_retry_at":1}
EOF
# Restart so the broker loads the planted file with attempts=max.
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null
wait "$broker_pid" 2>/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 1.0

inflight=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" inflight)
audit=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek audit 2>/dev/null)
ops=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek inbox-ops 2>/dev/null)

if printf '%s\n' "$inflight" | grep -q 'no in-flight' \
        && printf '%s\n' "$audit" | grep -q "msg_id=$ID" \
        && printf '%s\n' "$ops" | grep -q 'itester'; then
    tc_pass "TC51 max-attempts retry → escalation to audit + inbox-ops"
else
    tc_fail "TC51 escalation" "inflight: $(printf '%s\n' "$inflight" | head -2 | tr '\n' '|'); audit: $(printf '%s\n' "$audit" | head -2 | tr '\n' '|'); ops: $(printf '%s\n' "$ops" | head -2 | tr '\n' '|')"
fi
unset CLAUDE_BUS_ACK_TIMEOUT_MS

# ─────────────────────────────────────────────────────────────────────
# Phase 4f.1 tests — bus broadcast verb.

# Restart broker so state cleared by TC51 doesn't trip the next checks.
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker stop >/dev/null 2>&1
wait "$broker_pid" 2>/dev/null
rm -rf "$BROKER_STATE"
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broker run > /tmp/broker-itest.log 2>&1 &
broker_pid=$!
sleep 0.3

# TC52: broadcast fans into N inboxes.
echo "=== TC52: bus broadcast fans into matching inboxes ==="
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=ibcaster \
    "$BUS" broadcast itest-tag "fanned" --to ix1,ix2,ix3 >/dev/null
ok=0
for who in ix1 ix2 ix3; do
    p=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek inbox-$who)
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
echo "=== TC53: bus broadcast missing --to ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" broadcast tag body 2>/tmp/bus-itest.err
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
    "$BUS" enqueue itest-status "system green" >/dev/null
ok=0
for who in sa sb sc; do
    p=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek inbox-$who 2>&1)
    if printf '%s\n' "$p" | grep -q '| system green'; then
        ok=$(( ok + 1 ))
    fi
done
canonical=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek itest-status)
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
    "$BUS" enqueue itest-bb "first" >/dev/null
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=test \
    "$BUS" enqueue itest-bb "second" >/dev/null
p=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek itest-bb)
if printf '%s\n' "$p" | grep -q '| second' \
        && ! printf '%s\n' "$p" | grep -q '| first'; then
    tc_pass "TC56 blackboard peek shows only latest"
else
    tc_fail "TC56 blackboard" "peek: $(printf '%s\n' "$p" | tr '\n' '|')"
fi

# TC57: fetch is non-destructive on blackboard.
echo "=== TC57: blackboard fetch is non-destructive ==="
a=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" fetch itest-bb | grep '^|' | head -1)
b=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" fetch itest-bb | grep '^|' | head -1)
if [ -n "$a" ] && [ "$a" = "$b" ]; then
    tc_pass "TC57 blackboard fetch idempotent ($a)"
else
    tc_fail "TC57 blackboard fetch" "a='$a' b='$b'"
fi

# ─────────────────────────────────────────────────────────────────────
# Phase 4g tests — `bus body MSG_ID` (side-effect-free read by id).

# TC58: large body → bus body returns the full content and the topic
# cursor is unchanged afterward (the call must NOT advance / ack).
echo "=== TC58: bus body returns large body without cursor side effects ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" topic create itest-body \
    --kind work-queue >/dev/null 2>&1
# 2000 bytes of deterministic content (would have overflowed the old
# 4096-byte PIPE_BUF cap once framing + metadata is added; today's cap
# is 1 MiB so this stores inline in the topic log).
big_body=$(printf 'A%.0s' $(seq 1 2000))
body_id=$(CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
          "$BUS" enqueue itest-body "$big_body" 2>/dev/null)
cursor_p="$BROKER_STATE/cursors/itest-body/_default.cursor"
before=$([ -f "$cursor_p" ] && cat "$cursor_p" || echo "absent")
got=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" body "$body_id" 2>/tmp/bus-itest.err)
rc=$?
after=$([ -f "$cursor_p" ] && cat "$cursor_p" || echo "absent")
if [ "$rc" = "0" ] \
        && [ "$got" = "$big_body" ] \
        && [ "$before" = "$after" ]; then
    tc_pass "TC58 bus body large body, cursor stable ($before → $after)"
else
    tc_fail "TC58 bus body" "rc=$rc bytes=${#got}/${#big_body} before=$before after=$after err=$(cat /tmp/bus-itest.err | head -2)"
fi

# TC59: bus body on an unknown msg_id → rc=1 with error message.
echo "=== TC59: bus body unknown msg_id ==="
CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" body 0000000000000-ghost-ffff \
    >/tmp/bus-itest.out 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "1" ] && grep -q 'no such msg_id' /tmp/bus-itest.err; then
    tc_pass "TC59 bus body unknown msg_id → rc=1"
else
    tc_fail "TC59 bus body unknown" "rc=$rc err=$(cat /tmp/bus-itest.err | head -2)"
fi

# TC60: bus body without arg → usage on stderr, rc=2.
echo "=== TC60: bus body usage ==="
"$BUS" body 2>/tmp/bus-itest.err
rc=$?
if [ "$rc" = "2" ] && grep -q 'usage:.*body' /tmp/bus-itest.err; then
    tc_pass "TC60 bus body no-args usage"
else
    tc_fail "TC60 bus body usage" "rc=$rc"
fi

# TC61: enqueue a body across the old PIPE_BUF boundary. Anything past
# ~3.8 KiB used to hit "record size N exceeds atomic-write limit 4096".
# Now that the broker is the sole writer to topic logs (no PIPE_BUF
# atomicity needed), the cap is 1 MiB and 8 KiB sails through.
echo "=== TC61: enqueue 8 KiB body (regression for the 4096 cap) ==="
big_body=$(printf 'B%.0s' $(seq 1 8192))
CLAUDE_BUS_STATE=$BROKER_STATE CLAUDE_BUS_AGENT_ID=itester \
    "$BUS" enqueue inbox-itest-big "$big_body" \
    >/tmp/bus-itest.out 2>/tmp/bus-itest.err
rc=$?
got=$(CLAUDE_BUS_STATE=$BROKER_STATE "$BUS" peek inbox-itest-big \
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
