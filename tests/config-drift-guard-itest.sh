#!/usr/bin/env bash
# config-drift-guard-itest.sh — settings/hooks-shared/config-drift-guard.sh.
# The guard is the durable fix for the 2026-06-04 auri wedge: a never-crashing
# coordinator that materialized stale hooks at launch (events → /tmp, broker
# blind, inbox stalled) and never re-materialized because it never relaunched.
# The guard re-materializes the workspace's hook scripts from claude-bus's
# CURRENT main, rate-limited, overwrite-in-place, on PostToolUse.
#
# Cases: heal drift · rate-limit (no re-check within interval) · in-sync no-op
#        (aged stamp re-checks but writes nothing) · always exit 0.

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GUARD="$ROOT/settings/hooks-shared/config-drift-guard.sh"
[ -f "$GUARD" ] || { echo "drift-guard-itest: $GUARD missing"; exit 1; }
# The guard reads claude-bus's landed main via its git dir; need a real one.
BUS_ROOT="${CLAUDE_BUS_ROOT:-/home/sulin/claude-bus}"
[ -d "$BUS_ROOT/.git" ] || { echo "drift-guard-itest: no .git at $BUS_ROOT (set CLAUDE_BUS_ROOT)"; exit 1; }

T="$(mktemp -d /tmp/drift-guard-itest.XXXXXX)"
mkdir -p "$T/state" "$T/ws/settings/hooks"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }
trap 'rm -rf "$T"' EXIT

export CLAUDE_BUS_STATE="$T/state" CLAUDE_BUS_ROOT="$BUS_ROOT" \
       CLAUDE_BUS_AGENT_WORKSPACE="$T/ws" CLAUDE_BUS_AGENT_ID="guardtest"
unset CLAUDE_PROJECT_DIR 2>/dev/null || true
run() { bash "$GUARD" >/dev/null 2>&1; echo $?; }

echo "1. heal: a drifted log-event.sh is re-materialized from main"
printf '#!/usr/bin/env bash\nLOG_DIR=/tmp/claude-bus  # DRIFTED\n' \
  > "$T/ws/settings/hooks/log-event.sh"
ck "$(run)" "0" "guard exits 0"
grep -q '/tmp/claude-bus' "$T/ws/settings/hooks/log-event.sh" \
  && { ck drift sync "drifted /tmp LOG_DIR was healed"; } \
  || ck sync sync "drifted /tmp LOG_DIR was healed"
ck "$([ -s "$T/state/config-guard/guardtest.healed.log" ] && echo yes || echo no)" \
   "yes" "heal was audited to config-guard/<agent>.healed.log"

echo "2. rate-limit: a second run within the interval does nothing"
# corrupt the file again; the guard must NOT touch it (stamp is fresh)
printf 'DRIFT-AGAIN\n' > "$T/ws/settings/hooks/log-event.sh"
ck "$(run)" "0" "rate-limited run exits 0"
ck "$(cat "$T/ws/settings/hooks/log-event.sh")" "DRIFT-AGAIN" \
   "within interval: guard did NOT re-check (left the file alone)"

echo "3. in-sync no-op: aged stamp re-checks, but an up-to-date file is not rewritten"
# first heal it (age the stamp so the guard re-checks)
touch -d '20 minutes ago' "$T/state/config-guard/guardtest.checked"
run >/dev/null                                   # heals DRIFT-AGAIN → main
touch -d '20 minutes ago' "$T/state/config-guard/guardtest.checked"
mb=$(stat -c %Y "$T/ws/settings/hooks/log-event.sh")
ck "$(run)" "0" "in-sync run exits 0"
ma=$(stat -c %Y "$T/ws/settings/hooks/log-event.sh")
ck "$mb" "$ma" "in-sync file was NOT rewritten (cmp guard)"

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mDRIFT-GUARD OK: heal / rate-limit / in-sync-no-op all hold\033[0m"
else
  echo -e "\033[1mDRIFT-GUARD FAILED\033[0m"
fi
exit "$fail"
