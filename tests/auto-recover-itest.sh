#!/usr/bin/env bash
# auto-recover-itest.sh — P2 Phase A (observe-only) proof. The triage engine
# must FIRE its would-recover log on a genuinely stuck agent and stay SILENT on
# a healthy one (the false-positive shakedown). It ACTS nothing — we read the
# audit log straight off disk (a file read, not an RPC).
#
# Setup (seeded BEFORE boot so the boot tick's maybeAutoRecover sees it):
#   - hungagent: an OPEN turn (UserPromptSubmit, no Stop) 30 s ago + a stale
#     transcript (mtime 30 s old). With tiny budgets → hung-turn + wedged fire.
#   - healthyagent: an OPEN turn just NOW + a fresh transcript → neither fires.
# A fake zellij resolves both panes (list-panes) and returns an empty screen.

set -uo pipefail
BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
export CLAUDE_BUS_STATE="$(mktemp -d /tmp/auto-recover-itest.XXXXXX)"
export CLAUDE_BUS_STUCK_BUDGET_MS=2000   # hung-turn = turn open > 2x = 4 s
export CLAUDE_BUS_WEDGE_BUDGET_MS=2000   # transcript stale > 2 s
AUDIT="$CLAUDE_BUS_STATE/topics/audit.log"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }
hits() { grep -ac "would-recover agent=$1" "$AUDIT" 2>/dev/null || true; }

# Fake zellij: resolve both panes; empty screen on dump.
FAKE="$CLAUDE_BUS_STATE/fakebin"; mkdir -p "$FAKE"
cat > "$FAKE/zellij" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *list-panes*)
    cat <<'JSON'
{
  "tabs": [
    {
      "panes": [
        {
          "id": 1,
          "title": "hungagent",
          "is_plugin": false
        },
        {
          "id": 2,
          "title": "healthyagent",
          "is_plugin": false
        }
      ]
    }
  ]
}
JSON
    ;;
  *) : ;;  # dump-screen / write / send-keys → empty, exit 0
esac
exit 0
EOF
chmod +x "$FAKE/zellij"
export PATH="$FAKE:$PATH"

# Transcripts: hung one is stale (30 s old mtime), healthy one is fresh.
mkdir -p "$CLAUDE_BUS_STATE/transcripts"
HUNG_T="$CLAUDE_BUS_STATE/transcripts/hung.jsonl"
HEAL_T="$CLAUDE_BUS_STATE/transcripts/heal.jsonl"
printf '{}\n' > "$HUNG_T"; printf '{}\n' > "$HEAL_T"
touch -d '30 seconds ago' "$HUNG_T"   # stale

OLD_TS="$(date -u -d '30 seconds ago' +%FT%T.%3NZ)"
NOW_TS="$(date -u +%FT%T.%3NZ)"
cat > "$CLAUDE_BUS_STATE/events.jsonl" <<EOF
{"ts":"$OLD_TS","session":"t","agent":"hungagent","pane":"1","event":"UserPromptSubmit","payload":{"prompt":"do a thing","transcript_path":"$HUNG_T"}}
{"ts":"$NOW_TS","session":"t","agent":"healthyagent","pane":"2","event":"UserPromptSubmit","payload":{"prompt":"working","transcript_path":"$HEAL_T"}}
EOF

"$BUS" broker run >"$CLAUDE_BUS_STATE/broker.out" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
for _ in $(seq 1 50); do [ -S "$CLAUDE_BUS_STATE/broker.sock" ] && break; sleep 0.1; done
if [ ! -S "$CLAUDE_BUS_STATE/broker.sock" ]; then
  echo "broker did not come up:"; cat "$CLAUDE_BUS_STATE/broker.out"; exit 1
fi
echo "isolated broker up (pid $BPID); reading audit off disk, no RPC"

# Give the boot tick (which runs maybeAutoRecover once) time to write audit.
sleep 2

hung="$(hits hungagent)"
heal="$(hits healthyagent)"
ck "$([ "${hung:-0}" -ge 1 ] && echo fired)" "fired" \
   "would-recover FIRED on the stuck agent (count=$hung)"
ck "${heal:-0}" "0" \
   "would-recover SILENT on the healthy agent (count=$heal)"
# The stuck agent should name the relaunch action (wedged) — the new capability.
relaunch="$(grep -ac 'agent=hungagent signature=wedged action=relaunch' "$AUDIT" 2>/dev/null || true)"
ck "$([ "${relaunch:-0}" -ge 1 ] && echo yes)" "yes" \
   "stuck agent's would-recover names action=relaunch (wedged row; count=$relaunch)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mAUTO-RECOVER OBSERVE-ONLY CHECK PASSED\033[0m"
else
  echo -e "\033[1mAUTO-RECOVER OBSERVE-ONLY CHECK FAILED\033[0m"
  echo "would-recover lines:"; grep -a 'would-recover' "$AUDIT" 2>/dev/null
  echo "broker.out:"; cat "$CLAUDE_BUS_STATE/broker.out"
fi
exit "$fail"
