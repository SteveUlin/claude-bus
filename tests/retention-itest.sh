#!/usr/bin/env bash
# retention-itest.sh — integration test for in-tick log retention (D1+D2).
# Spins up an ISOLATED broker in a throwaway $STATE
# (never the live one) with a tiny byte cap + a fast trim interval, and
# proves the file-backed path the unit tests can't reach:
#
#   1. a non-guaranteed topic (append-log) over the byte cap gets its head
#      trimmed — oldest records dropped, newest survive;
#   2. a guaranteed agent-inbox with UNREAD mail is NEVER trimmed (the
#      delivery-guarantee clamp), even far over the cap;
#   3. cursor rebase keeps consumption seamless: after a consumer advances
#      and the consumed head is trimmed, the next fetch returns the correct
#      next-unread record (no skip, no dup, no re-serve of consumed mail).
#
# Cleans up broker + state on exit. Touches nothing live.

set -uo pipefail

BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/retention-itest.XXXXXX)"
export CLAUDE_BUS_TRIM_INTERVAL_MS=300    # sweep promptly (default 60s)
export CLAUDE_BUS_TOPIC_MAX_BYTES=600     # tiny cap to force a head trim
export CLAUDE_BUS_EVENTS_MAX_BYTES=4096   # tiny cap for the events.jsonl rewrite
fail=0

note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ck() {  # ck ACTUAL EXPECTED LABEL
  if [ "$1" = "$2" ]; then echo "  ok: $3"; else
    echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi
}
# Count occurrences of a marker in a topic's records, read from a private
# observer cursor so the consuming _default cursor never skews the view.
seen() { "$BUS" msg peek "$1" --consumer obs 2>/dev/null | grep -c "$2"; }

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

note "1. append-log over the byte cap is head-trimmed (oldest dropped)"
"$BUS" topic create logtest --kind append-log >/dev/null
for i in $(seq -w 1 20); do
  "$BUS" msg enqueue logtest "rec-$i-paddingpaddingpaddingpaddingpaddingpad" >/dev/null
done
sleep 0.8  # let a trim sweep run
surv="$(seen logtest 'rec-')"
ck "$([ "$surv" -lt 20 ] && [ "$surv" -gt 0 ] && echo trimmed)" "trimmed" \
   "append-log shrank under the cap (survivors=$surv of 20)"
ck "$(seen logtest 'rec-20')" "1" "newest record (rec-20) survives"
ck "$(seen logtest 'rec-01')" "0" "oldest record (rec-01) was dropped"

note "2. guaranteed inbox with UNREAD mail is never trimmed (clamp)"
# No pane for zinbox → broker can't deliver → cursor never advances →
# min_cursor=0 → clamp forbids any trim. All 20 must survive the cap.
for i in $(seq -w 1 20); do
  "$BUS" msg mail zinbox "mail-$i-paddingpaddingpaddingpaddingpaddingpadding" >/dev/null
done
sleep 0.8
ck "$(seen inbox-zinbox 'mail-')" "20" "all 20 unread inbox records preserved"

note "2b. reserved sink (inbox-ops) IS trimmed despite a stuck cursor (CRIT #3)"
# inbox-ops is agent-inbox with no live consumer (nothing drains it), so the
# clamp would pin retention forever. The reserved-sink exemption lets the
# size cap trim it — oldest dropped, newest kept — unlike zinbox above.
for i in $(seq -w 1 20); do
  "$BUS" msg mail ops "ops-$i-paddingpaddingpaddingpaddingpaddingpaddingpad" >/dev/null
done
sleep 0.8
surv_ops="$(seen inbox-ops 'ops-')"
ck "$([ "$surv_ops" -lt 20 ] && [ "$surv_ops" -gt 0 ] && echo trimmed)" "trimmed" \
   "inbox-ops trimmed past its stuck cursor ($surv_ops of 20 survive)"
ck "$(seen inbox-ops 'ops-20')" "1" "newest ops record (ops-20) survives"
ck "$(seen inbox-ops 'ops-01')" "0" "oldest ops record (ops-01) was dropped"

note "3. cursor rebase keeps consumption seamless across a trim"
for i in $(seq -w 1 10); do
  "$BUS" msg mail zseq "seq-$i-paddingpaddingpaddingpaddingpaddingpaddingpad" >/dev/null
done
# Consume the first 5 (advances the _default cursor to seq-06's offset).
for _ in $(seq 1 5); do "$BUS" msg fetch inbox-zseq >/dev/null; done
sleep 0.8  # trim: clamp caps the cut at the consumer cursor → drops seq-01..05
# The clamp drops exactly the consumed prefix regardless of record size.
ck "$(seen inbox-zseq 'seq-01')" "0" "consumed head (seq-01) trimmed away"
ck "$(seen inbox-zseq 'seq-06')" "1" "first unread (seq-06) preserved"
ck "$(seen inbox-zseq 'seq-10')" "1" "newest unread (seq-10) preserved"
# The decisive check: the rebased cursor still points at the right record.
next="$("$BUS" msg fetch inbox-zseq 2>/dev/null | grep -c 'seq-06')"
ck "$next" "1" "next fetch returns seq-06 (cursor rebased, no skip/dup)"

note "4. events.jsonl over the cap is rewritten to a recent tail"
# events.jsonl is advisory (hooks write it; no agents here, so seed it).
# The broker tick is RPC-driven, so a poke RPC drives the sweep — exactly
# how viewer-poll traffic drives it in the live fleet.
for i in $(seq -w 1 400); do
  printf '{"ts":"x","agent":"a","event":"Stop","payload":{"m":"evt-%s"}}\n' "$i"
done > "$CLAUDE_BUS_STATE/events.jsonl"
before_evt="$(stat -c%s "$CLAUDE_BUS_STATE/events.jsonl")"
# The trim runs on a post-RPC tick, but the 300ms trim-gate may be closed
# right after section 3's rapid fetches, and idle ticks are unreliable. So
# drive pokes spaced PAST the gate until the sweep fires (bounded ~3s) —
# this mirrors the live fleet's steady viewer-poll cadence rather than
# betting one tick lands gate-open.
after_evt="$before_evt"
for _ in $(seq 1 8); do
  "$BUS" msg mail poke "drive a tick" >/dev/null
  sleep 0.4
  after_evt="$(stat -c%s "$CLAUDE_BUS_STATE/events.jsonl")"
  [ "$after_evt" -lt "$before_evt" ] && break
done
ck "$([ "$after_evt" -lt "$before_evt" ] && [ "$after_evt" -le 4096 ] && echo ok)" \
   "ok" "events.jsonl shrank under the 4096 cap ($before_evt -> $after_evt)"
ck "$(grep -c 'evt-400' "$CLAUDE_BUS_STATE/events.jsonl")" "1" "newest event line kept"
ck "$(grep -c 'evt-001' "$CLAUDE_BUS_STATE/events.jsonl")" "0" "oldest event line dropped"
# Every retained line must be whole (rewrite snaps to a line boundary).
partial="$(head -1 "$CLAUDE_BUS_STATE/events.jsonl" | grep -c '^{.*}$')"
ck "$partial" "1" "first retained line is a complete JSON record (no torn head)"

echo
if [ "$fail" = 0 ]; then echo -e "\033[1mALL RETENTION INTEGRATION CHECKS PASSED\033[0m"; else
  echo -e "\033[1mSOME CHECKS FAILED\033[0m"; fi
exit "$fail"
