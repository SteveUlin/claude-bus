#!/usr/bin/env bash
# monitor-truth-itest.sh — independent certification that `bus monitor` does
# NOT lie: its CTX denominator + numerator and the EFFORT column faithfully
# reflect the authoritative statusline capture, never a synthesized constant
# or a stale launch flag. (sulin's "the monitor must not lie" mandate; auri
# dispatched the cert to bast, kvothe built the fix @ 27af25bc / Path 1.)
#
# METHOD: feed a KNOWN fixture into the stable contract file
# `$STATE/statusline/<agent>.json` (kvothe's producer-v1 wrapper writes it from
# statusline stdin; we assert against the FILE shape, not the wrapper, so this
# survives a producer swap — e.g. OTel v2). Render ONE `bus monitor` frame,
# strip ANSI, and assert the agent's row reflects the fixture. A fake zellij
# gives the agent a live pane so it renders (else it's GONE/filtered); the
# statusline fixture is the source under test.
#
# WHAT THE MONITOR DOES (sub_monitor contextStatsFor / formatCtx / formatEffort,
# 27af25bc): reads the capture first; CTX = "{used_percentage}%/{window}" with
# window = context_window_size (formatCtxSize: 1e6→"1M", 200000→"200K"); EFFORT
# = effort_level verbatim, or "—" when empty (NOT-CAPTURED: pre-relaunch, or a
# model with no effort field — NOT a harness gap per sulin; .effort.level is a
# real, live statusline field).
#
# Gotchas baked in (project_broker_itest_gotchas + project_fake_zellij_delivery
# _harness): isolated $STATE; assert on the rendered frame; wait only on named
# pids. Run: BUS=/path/to/bin/bus bash tests/monitor-truth-itest.sh

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUS="${BUS:-$ROOT/bin/bus}"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/monitor-truth-itest.XXXXXX)"
AGENT="montest"
FAKE="$CLAUDE_BUS_STATE/fakebin"
mkdir -p "$FAKE" "$CLAUDE_BUS_STATE/statusline"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# Fake zellij: a live pane for $AGENT so it renders as a live agent (a GONE
# agent is filtered from the monitor list).
cat > "$FAKE/zellij" <<FAKEEOF
#!/usr/bin/env bash
case "\$*" in
  *list-panes*) printf '%s\n' '{' '  "id": 1,' '  "is_plugin": false,' '  "title": "$AGENT"' '}';;
  *dump-screen*) printf '%s\n' "> " "-- INSERT --  bypass permissions on";;
esac
exit 0
FAKEEOF
chmod +x "$FAKE/zellij"; export PATH="$FAKE:$PATH"

# Seed a Stop so the agent is known + idle (renders).
printf '%s\n' "{\"ts\":\"2026-06-02T07:00:00.000Z\",\"session\":\"t\",\"agent\":\"$AGENT\",\"pane\":\"1\",\"event\":\"Stop\",\"payload\":{}}" \
  > "$CLAUDE_BUS_STATE/events.jsonl"

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1; done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID), agent=$AGENT, binary=$("$BUS" version 2>/dev/null | grep -oE '[0-9a-f]{7}' | head -1)"

# Write the stable-contract fixture: window tokens pct effort  (model fixed).
fixture() {
  printf '{"agent":"%s","ts":1780000000000,"model_id":"claude-opus-4-8","model_display":"Opus 4.8","context_window_size":%s,"total_input_tokens":%s,"used_percentage":%s,"effort_level":"%s","exceeds_200k":false}' \
    "$AGENT" "$1" "$2" "$3" "$4" > "$CLAUDE_BUS_STATE/statusline/$AGENT.json"
}
# Capture the agent's ANSI-stripped monitor row (one frame).
row() {
  "$BUS" state >/dev/null 2>&1; sleep 0.2
  timeout 2 "$BUS" monitor 2>&1 \
    | sed 's/\x1b\[[0-9;]*[mGKHABCDJ]//g' \
    | grep -E "(^|[[:space:]])$AGENT[[:space:]]" | head -1
}

echo "1. CTX DENOMINATOR is the REAL per-model window from the file, not a constant"
fixture 1000000 500000 37 high; r=$(row)
ck "$(printf '%s' "$r" | grep -cE '37%/1M' || true)" "1" "window=1,000,000 renders CTX '37%/1M'"
fixture 200000 100000 50 high;  r=$(row)
ck "$(printf '%s' "$r" | grep -cE '50%/200K' || true)" "1" "window=200,000 renders CTX '50%/200K' (denominator TRACKS the file)"

echo "2. CTX NUMERATOR (pct) traces to used_percentage, not synthesized"
fixture 1000000 880000 88 high; r=$(row)
ck "$(printf '%s' "$r" | grep -cE '88%/1M' || true)" "1" "used_percentage=88 renders '88%' verbatim"

echo "3. EFFORT column reflects LIVE effort, and shows '—' when not-captured (never a faked launch flag)"
fixture 1000000 500000 37 high; r=$(row)
ck "$(printf '%s' "$r" | grep -cE 'opus-4-8[[:space:]]+high[[:space:]]+37%/1M' || true)" "1" "effort_level=high -> EFFORT cell 'high'"
fixture 1000000 500000 37 max;  r=$(row)
ck "$(printf '%s' "$r" | grep -cE 'opus-4-8[[:space:]]+max[[:space:]]+37%/1M' || true)" "1" "live change to effort_level=max -> EFFORT cell 'max' (not the stale launch flag)"
fixture 1000000 500000 37 "";   r=$(row)
ck "$(printf '%s' "$r" | grep -cE 'opus-4-8[[:space:]]+—[[:space:]]+37%/1M' || true)" "1" "effort_level='' -> EFFORT cell '—' (not-captured, never a faked launch flag)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mMONITOR-TRUTH CERTIFIED: CTX window + pct + EFFORT all reflect the authoritative file\033[0m"
else
  echo -e "\033[1mMONITOR-TRUTH FAILED: the display does not faithfully reflect the source\033[0m"
  echo "last row: [$r]"
fi
exit "$fail"
