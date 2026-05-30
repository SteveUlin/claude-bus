#!/usr/bin/env bash
# off-tty-itest.sh — end-to-end integration test for OFF-TTY delivery,
# now the FLEET DEFAULT (roadmap 2.1, Fork B). Spins up an ISOLATED
# broker in a throwaway $STATE (never the live one) and proves:
#   - every agent is off-TTY by DEFAULT (drain -> additionalContext);
#   - the durable TTY opt-out set (compiled-in "comms" + the
#     $CLAUDE_BUS_TTY_AGENTS env list) stays on the push path (drain
#     returns empty — no double-delivery);
#   - presence + consume-as-ack semantics intact.
# Cleans up the broker + state on exit. Touches nothing live.

set -uo pipefail

# Resolve the binary from THIS tree (workspace-relative), not a hardcoded
# default-tree path — so a workspace build is tested, not the landed one.
BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/off-tty-itest.XXXXXX)"
export CLAUDE_BUS_TTY_AGENTS="tester2"  # env opt-out (comms is compiled-in)
fail=0

note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ck() {  # ck ACTUAL EXPECTED LABEL
  if [ "$1" = "$2" ]; then echo "  ok: $3"; else
    echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi
}

# Isolated broker — inherits CLAUDE_BUS_TTY_AGENTS so its drain RPC sees
# the opt-out list. NOT nohup/setsid/disown.
"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT

for _ in $(seq 1 50); do
  [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break
  sleep 0.1
done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up; output:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID), state $CLAUDE_BUS_STATE"

note "1. a normal agent is off-TTY by DEFAULT — no flag needed"
"$BUS" msg mail tester "first off-tty message" >/dev/null
"$BUS" msg mail tester "second one" >/dev/null

note "2. drain -> additionalContext JSON carrying BOTH (no TTY write)"
out="$("$BUS" msg drain tester UserPromptSubmit)"
echo "$out"
echo "$out" | grep -q '"hookEventName":"UserPromptSubmit"'; ck "$?" 0 "emits additionalContext"
echo "$out" | grep -q 'first off-tty message'; ck "$?" 0 "carries first record"
echo "$out" | grep -q 'second one'; ck "$?" 0 "carries second record"

note "3. drain again -> cursor advanced on consume = the ack; nothing left"
out2="$("$BUS" msg drain tester)"
ck "$out2" "" "second drain empty (consume advanced the cursor)"

note "4. presence gate: attach, mail, drain DEFERS; detach delivers"
mkdir -p "$CLAUDE_BUS_STATE/presence"; touch "$CLAUDE_BUS_STATE/presence/tester"
"$BUS" msg mail tester "typed while you held the keyboard" >/dev/null
out3="$("$BUS" msg drain tester)"; ck "$out3" "" "drain defers while attached"
rm -f "$CLAUDE_BUS_STATE/presence/tester"
out4="$("$BUS" msg drain tester)"
echo "$out4" | grep -q 'typed while you held the keyboard'; ck "$?" 0 "deferred record delivers after detach"

note "5. TTY OPT-OUT (compiled-in 'comms') stays on the push path"
"$BUS" msg mail comms "comms is human-facing" >/dev/null
out5="$("$BUS" msg drain comms)"
ck "$out5" "" "comms drain empty (stays on TTY push, no double-delivery)"

note "6. TTY OPT-OUT via \$CLAUDE_BUS_TTY_AGENTS (tester2) stays on push"
"$BUS" msg mail tester2 "env opt-out" >/dev/null
out6="$("$BUS" msg drain tester2)"
ck "$out6" "" "env-listed agent drains nothing"

note "7. per-agent isolation: another default agent drains only its own"
"$BUS" msg mail tester3 "for tester3 only" >/dev/null
out7="$("$BUS" msg drain tester3)"
echo "$out7" | grep -q 'for tester3 only'; ck "$?" 0 "tester3 drains its own record"
echo "$out7" | grep -q 'off-tty message'; ck "$?" 1 "tester3 does NOT see tester's mail"

# ── C4 objective verification: doorbell + strand-watchdog ──
# The doorbell/strand-watchdog (delivery.cpp, maybeWakeIdleOffTty) only
# acts on agents with a LIVE zellij pane — paneId() shells out to
# `zellij action list-panes`, which returns nothing in this headless
# subprocess sandbox, so the wake/strand EMISSION path never runs here.
# Reproducing the doorbell-FIRES and strand-FIRES cases needs the
# real-zellij + stub-pane harness (Option B in docs/broker-test-sandbox.md).
# That positive reproduction lives, today, in the pure-logic unit test
# tests/unit/test_doorbell_readiness.cpp — which proves the wake gate
# (process==Alive && turn==Ready) and pins the post-compaction strand.
# What this sandbox CAN assert objectively:

note "8. exactly one delivery path per agent (no double-delivery)"
# Off-TTY default: drain is the path, push is skipped. TTY opt-out:
# push is the path, drain returns empty. Asserted as a matrix so a
# regression that puts an agent on BOTH paths fails loudly.
"$BUS" msg mail pathtest "single-path probe" >/dev/null
op="$("$BUS" msg drain pathtest)"
echo "$op" | grep -q 'single-path probe'; ck "$?" 0 "off-TTY agent: mail arrives via DRAIN"
op2="$("$BUS" msg drain pathtest)"
ck "$op2" "" "off-TTY agent: consumed once, not redelivered"
"$BUS" msg mail comms "comms single-path probe" >/dev/null
oc="$("$BUS" msg drain comms)"
ck "$oc" "" "TTY agent (comms): drain empty — delivered by PUSH only"

note "9. strand-watchdog: SILENT under healthy delivery (no false alarm)"
# Every record enqueued above was drained cleanly (has_mail->false), so
# the watchdog must not have emitted a single doorbell-strand alarm. A
# spurious alarm leaking into the audit topic during normal drain ops
# would be a false-positive regression.
audit="$("$BUS" msg peek audit --limit 200 2>/dev/null)"
echo "$audit" | grep -q 'doorbell-strand'; ck "$?" 1 "no doorbell-strand audit under healthy delivery"
echo "$audit" | grep -q 'protocol:.*doorbell\b'; ck "$?" 1 "no doorbell wake fired (no live pane in sandbox)"

# TODO(C1, kvothe — landed): now that C1's cursor-state assertions are in,
# a follow-up could add the strand-clock reproduction here: enqueue, hold a
# pane unattended past CLAUDE_BUS_STRAND_MS, and assert the inbox cursor has
# NOT advanced (mail genuinely undelivered) at the moment the doorbell-strand
# alarm fires — distinguishing a true strand from a drained-but-lost record.
# Needs the Option-B zellij-pane harness (paneId is headless-blind here).

note "10. compact-SessionStart does NOT consume mail (the strand fix)"
# /compact's additionalContext never surfaces, so inbox-drain.sh must SKIP
# draining on SessionStart(source=compact) — leaving the mail QUEUED for the
# doorbell (which now wakes a Compacting agent) to deliver via a live turn.
# Resume/clear SessionStarts must still drain. Drives the hook directly.
HOOK="$(cd "$(dirname "$0")/.." && pwd)/settings/hooks-shared/inbox-drain.sh"
export CLAUDE_BUS_AGENT_ID=compactee
"$BUS" msg mail compactee "must survive /compact" >/dev/null
printf '%s' '{"hook_event_name":"SessionStart","source":"compact"}' \
  | "$HOOK" SessionStart >/dev/null 2>&1
hc=$?
surv="$("$BUS" msg drain compactee UserPromptSubmit)"
echo "$surv" | grep -q 'must survive /compact'
ck "$?" 0 "compact SessionStart skips drain — mail still queued"
ck "$hc" 0 "compact-skip exits clean"
"$BUS" msg mail compactee "resume should deliver" >/dev/null
rout="$(printf '%s' '{"hook_event_name":"SessionStart","source":"resume"}' \
  | "$HOOK" SessionStart 2>/dev/null)"
echo "$rout" | grep -q 'resume should deliver'
ck "$?" 0 "resume SessionStart still drains (skip is compact-only)"
unset CLAUDE_BUS_AGENT_ID

echo
if [ "$fail" = 0 ]; then echo -e "\033[1mALL OFF-TTY INTEGRATION CHECKS PASSED\033[0m"; else
  echo -e "\033[1mSOME CHECKS FAILED\033[0m"; fi
exit "$fail"
