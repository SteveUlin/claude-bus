#!/usr/bin/env bash
# demo.sh — exercise the off-TTY drain prototype end-to-end in a fully
# isolated $STATE. Touches nothing live: no broker, no real fleet state,
# no settings.json. Just shows the four behaviors that matter.

set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/off-tty-demo.XXXXXX)"
export CLAUDE_BUS_AGENT_ID="demo"
trap 'rm -rf "$CLAUDE_BUS_STATE"' EXIT

say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

say "isolated state: $CLAUDE_BUS_STATE"

say "1. broker writes two records (no TTY touched)"
"$HERE/sim-broker-write.sh" demo "the cache invalidation idea looks right" alice
"$HERE/sim-broker-write.sh" demo "ship it after tests pass" bob

say "2. UserPromptSubmit drain -> emits BOTH as additionalContext"
"$HERE/inbox-drain.sh" UserPromptSubmit

say "3. drain again -> idempotent, emits nothing (cursor + msg_id dedup)"
out=$("$HERE/inbox-drain.sh" UserPromptSubmit)
[ -z "$out" ] && echo "(no output — correct)" || echo "UNEXPECTED: $out"

say "4. human attaches (presence sentinel) + broker writes one more"
mkdir -p "$CLAUDE_BUS_STATE/presence"
touch "$CLAUDE_BUS_STATE/presence/demo"
"$HERE/sim-broker-write.sh" demo "typed while you had the keyboard" carol

say "5. drain while attached -> presence gate DEFERS, emits nothing"
out=$("$HERE/inbox-drain.sh" UserPromptSubmit)
[ -z "$out" ] && echo "(no output — correctly deferred)" || echo "UNEXPECTED: $out"

say "6. human detaches -> the deferred record now delivers"
rm -f "$CLAUDE_BUS_STATE/presence/demo"
"$HERE/inbox-drain.sh" UserPromptSubmit

say "done (isolated state cleaned up)"
