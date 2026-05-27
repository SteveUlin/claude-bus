#!/usr/bin/env bash
# Phase 4 playground — clean isolated broker; round-trips through every
# subcommand that exists today. Read along, or `bash playground.sh` to
# run it; nothing here touches your real bus session.

set -uo pipefail

BUS=/home/sulin/claude-bus/bin/bus
export CLAUDE_BUS_STATE=/tmp/bus-play
export CLAUDE_BUS_AGENT_ID=playground

step() { printf "\n\033[1;36m── %s ──\033[0m\n" "$1"; }
run()  { printf "\033[2m$ %s\033[0m\n" "$*"; "$@"; }

# ── 0. clean slate ────────────────────────────────────────────────
rm -rf "$CLAUDE_BUS_STATE"
"$BUS" broker stop >/dev/null 2>&1 || true
sleep 0.1

step "0. one-shot subcommands (no broker needed)"
run "$BUS" version
run "$BUS" pane-id elodin       || true
run "$BUS" pane-state elodin | head -3

# ── 1. start the broker daemon ───────────────────────────────────
step "1. start broker daemon"
"$BUS" broker run > /tmp/bus-play.broker.log 2>&1 &
broker_pid=$!
sleep 0.3
run "$BUS" broker status

# ── 2. topic registry ────────────────────────────────────────────
step "2. topic registry"
run "$BUS" topic create build-tasks --kind work-queue
run "$BUS" topic create heartbeats  --kind pubsub
run "$BUS" topic list

step "  show one topic with all its fields"
run "$BUS" topic show build-tasks

# ── 3. publish / consume on an explicit topic ─────────────────────
step "3. publish + consume a work queue"
ID_A=$("$BUS" enqueue build-tasks "build the thing")
ID_B=$("$BUS" enqueue build-tasks "test the thing")
# Flags come AFTER body: `bus enqueue TOPIC BODY [--flag VALUE]...`
ID_C=$("$BUS" enqueue build-tasks "ship the thing" --protocol task --ttl 60000)
printf "ids: %s  %s  %s\n" "$ID_A" "$ID_B" "$ID_C"

step "  peek without consuming"
run "$BUS" peek build-tasks --limit 2

step "  fetch — advances cursor"
run "$BUS" fetch build-tasks
echo "(cursor moved; next fetch returns task #2)"
run "$BUS" fetch build-tasks

# ── 4. auto-created agent inboxes ─────────────────────────────────
step "4. bus mail / bus slash auto-create per-agent topics"
run "$BUS" mail  bast    "hello bast — from the playground"
run "$BUS" slash kvothe  "/clear"
run "$BUS" topic list

step "  the slash record carries the right defaults"
run "$BUS" peek commands-kvothe

# ── 5. broker view of agent state ────────────────────────────────
step "5. ask the broker who's around"
run "$BUS" state elodin

# ── 6. restart-survival ──────────────────────────────────────────
step "6. broker restart — topic data survives"
echo "queue something fresh first so the cursor isn't already past EOF:"
run "$BUS" enqueue build-tasks "queued before restart"
run "$BUS" broker stop
wait "$broker_pid" 2>/dev/null
echo "(broker stopped — on-disk artifacts:)"
ls "$CLAUDE_BUS_STATE/topics" 2>/dev/null
ls "$CLAUDE_BUS_STATE/cursors/build-tasks" 2>/dev/null

"$BUS" broker run > /tmp/bus-play.broker.log 2>&1 &
broker_pid=$!
sleep 0.2
echo "after restart, the unfetched record is still there:"
run "$BUS" peek build-tasks

# ── 7. teardown ──────────────────────────────────────────────────
step "7. teardown"
"$BUS" broker stop
wait "$broker_pid" 2>/dev/null
rm -rf "$CLAUDE_BUS_STATE"
echo "done."
