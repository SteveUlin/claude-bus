#!/usr/bin/env bash
# recover-clear-itest.sh — P2 Phase B increment (c): R1 idle-context → clear
# MIGRATED into the recovery engine. Two scenarios, each an isolated broker:
#
#   SOFT mode — the engine OWNS clearing: enqueues /clear through the breaker/
#     backoff ledger, persists the ledger, excludes comms/primary, and the
#     standalone maybeAutoClear steps aside (no double-clear, no auto-clear row).
#   OBSERVE mode (the default) — ZERO behavior change: the engine only logs
#     would-recover; the standalone maybeAutoClear is the actor (auto-clear row).
#
# Seeded BEFORE boot so the boot tick's maybeAutoRecover/maybeAutoClear see it:
#   - idleagent: a Stop event 65 s ago, empty inbox, no in-flight (idle > 60 s).
#   - comms: same idle shape, role-excluded → must NOT clear in either mode.
# A fake zellij resolves both panes and returns an empty screen.

set -uo pipefail
BUS="$(cd "$(dirname "$0")/.." && pwd)/bin/bus"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }
gc() { local n; n="$(grep -ac "$1" "$2" 2>/dev/null)"; echo "${n:-0}"; }  # 0 if missing
# Count ACTED recover rows — exclude would-recover (a superstring of "recover").
gcr() { grep -a "$1" "$2" 2>/dev/null | grep -avc 'would-recover'; }

# Boot an isolated broker in a given mode; seed two idle agents. Sets globals
# DIR + BPID (no command-substitution subshell, so they survive). Caller kills
# BPID + rms DIR.
boot() {
  local mode="$1"
  DIR="$(mktemp -d /tmp/recover-clear-itest.XXXXXX)"
  local dir="$DIR"
  local fake="$dir/fakebin"; mkdir -p "$fake"
  cat > "$fake/zellij" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *list-panes*)
    cat <<'JSON'
{ "tabs": [ { "panes": [
  { "id": 1, "title": "idleagent", "is_plugin": false },
  { "id": 2, "title": "comms", "is_plugin": false }
] } ] }
JSON
    ;;
  *) : ;;
esac
exit 0
EOF
  chmod +x "$fake/zellij"
  local old; old="$(date -u -d '65 seconds ago' +%FT%T.%3NZ)"
  cat > "$dir/events.jsonl" <<EOF
{"ts":"$old","session":"t","agent":"idleagent","pane":"1","event":"Stop","payload":{}}
{"ts":"$old","session":"t","agent":"comms","pane":"2","event":"Stop","payload":{}}
EOF
  CLAUDE_BUS_STATE="$dir" CLAUDE_BUS_AUTO_RECOVERY="$mode" \
    CLAUDE_BUS_AUTO_CLEAR_MIN=1 PATH="$fake:$PATH" \
    "$BUS" broker run >"$dir/broker.out" 2>&1 &
  BPID=$!
  for _ in $(seq 1 50); do [ -S "$dir/broker.sock" ] && break; sleep 0.1; done
  if [ ! -S "$dir/broker.sock" ]; then
    echo "broker did not come up:"; cat "$dir/broker.out"; kill "$BPID" 2>/dev/null
    exit 1
  fi
  sleep 2  # boot tick runs the scans once
}

# ── Scenario 1: SOFT — the engine acts ──────────────────────────────────────
echo "[soft] engine owns idle-context clearing"
boot soft; SOFT_PID=$BPID
AUDIT="$DIR/topics/audit.log"
ck "$([ "$(gc '/clear' "$DIR/topics/commands-idleagent.log")" -ge 1 ] && echo yes)" \
   "yes" "engine enqueued /clear to commands-idleagent"
ck "$([ "$(gcr 'recover agent=idleagent signature=idle-context action=clear' "$AUDIT")" -ge 1 ] && echo yes)" \
   "yes" "audit has acted 'recover ... idle-context ... clear'"
led_ok=no
if [ -f "$DIR/recovery/idleagent.json" ] && \
   grep -q 'idle-context' "$DIR/recovery/idleagent.json" 2>/dev/null; then led_ok=yes; fi
ck "$led_ok" "yes" "recovery ledger persisted with idle-context sig"
ck "$(gc '/clear' "$DIR/topics/commands-comms.log")" "0" "comms role-excluded — no /clear"
ck "$(gcr 'recover agent=comms' "$AUDIT")" "0" "comms role-excluded — no recover row"
ck "$(gc 'auto-clear agent=idleagent' "$AUDIT")" "0" "maybeAutoClear stood aside (no auto-clear row)"
kill "$SOFT_PID" 2>/dev/null; wait "$SOFT_PID" 2>/dev/null; rm -rf "$DIR"

# ── Scenario 2: OBSERVE — the default, no behavior change ────────────────────
echo "[observe] default: standalone auto-clear acts, engine only observes"
boot observe; OBS_PID=$BPID
AUDIT="$DIR/topics/audit.log"
ck "$([ "$(gc 'auto-clear agent=idleagent' "$AUDIT")" -ge 1 ] && echo yes)" \
   "yes" "standalone maybeAutoClear acted (auto-clear row present)"
ck "$(gcr 'recover agent=idleagent signature=idle-context action=clear' "$AUDIT")" "0" \
   "engine did NOT act (no recover row at observe)"
ck "$([ "$(gc 'would-recover agent=idleagent signature=idle-context' "$AUDIT")" -ge 1 ] && echo yes)" \
   "yes" "engine logged would-recover idle-context (observe)"
kill "$OBS_PID" 2>/dev/null; wait "$OBS_PID" 2>/dev/null; rm -rf "$DIR"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mRECOVER-CLEAR (R1 MIGRATION) CHECK PASSED\033[0m"
else
  echo -e "\033[1mRECOVER-CLEAR (R1 MIGRATION) CHECK FAILED\033[0m"
fi
exit "$fail"
