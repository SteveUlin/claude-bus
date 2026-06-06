#!/usr/bin/env bash
# continuity-itest.sh — the broker side of the reporting-truth continuity clamp
# (kvothe's part (a) consumer + elodin's loop-plane detector). Isolated broker.
# Proves the state RPC floors event-age at max(systemBootMs(), continuity_since_ms):
#   - a 12-min-old open turn (no tool) reads QUIET by age;
#   - once $STATE/continuity.ms is set to ~now (a resume instant the loop would
#     record on a Δwall−Δmono jump), the SAME agent reads WORKING — the resume
#     can't flip it red on staleness alone.
# Detection itself (the wall-jump) mirrors recovery's §6.1a tested logic; here we
# verify the exposure + clamp wiring. Touches nothing live; cleans up on exit.

set -uo pipefail

BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/continuity-itest.XXXXXX)"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }
turn_of() { "$BUS" state 2>/dev/null | awk '$1=="ag"{print $2}'; }

FAKE="$CLAUDE_BUS_STATE/fakebin"; mkdir -p "$FAKE"
cat > "$FAKE/zellij" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *list-panes*)
    cat <<'JSON'
{
  "tabs": [ { "panes": [ { "id": 1, "title": "ag", "is_plugin": false } ] } ]
}
JSON
    ;;
  *) : ;;
esac
exit 0
EOF
chmod +x "$FAKE/zellij"; export PATH="$FAKE:$PATH"

# ag: an open turn (no tool) 12 min ago → stale by age → Quiet.
OLD_TS="$(date -u -d '12 minutes ago' +%FT%T.%3NZ)"
printf '{"ts":"%s","session":"t","agent":"ag","pane":"1","event":"UserPromptSubmit","payload":{"prompt":"x"}}\n' "$OLD_TS" \
  > "$CLAUDE_BUS_STATE/events.jsonl"

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1; done
[ -S "$CLAUDE_BUS_STATE/broker.sock" ] || { echo "broker down:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1; }

printf '\n\033[1m== without continuity.ms: stale by age ==\033[0m\n'
ck "$(turn_of)" "QUIET" "12-min-old open turn reads QUIET (stale by age)"

printf '\n\033[1m== continuity.ms = now: age floored, no longer stale ==\033[0m\n'
printf '%s' "$(($(date +%s%3N)))" > "$CLAUDE_BUS_STATE/continuity.ms"
ck "$(turn_of)" "WORKING" "same agent reads WORKING (event-age floored at resume)"

echo
if [ "$fail" = 0 ]; then echo -e "\033[1mCONTINUITY CLAMP CHECK PASSED\033[0m"; else
  echo -e "\033[1mCONTINUITY CLAMP CHECK FAILED\033[0m"; fi
exit "$fail"
