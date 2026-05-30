#!/usr/bin/env bash
# config-equals-main.sh — C4 objective verification (c): every agent's
# materialized Claude Code config EQUALS landed main, by construction.
#
# This is the proof for 1b (agent-launch materializes config from landed
# main via `jj --ignore-working-copy`, staleness-immune). Unlike the
# other C4 checks this runs against the LIVE repo + workspaces (read-only,
# touches nothing) — it answers "is what every agent actually loaded the
# same as main?", the question that the stale-canonical bug kept failing.
#
# For each workspace under .workspaces/<agent>:
#   - .claude/settings.json        == main:settings/claude-settings.json
#   - settings/hooks/<each>.sh      == main:settings/hooks-shared/<each>.sh
#
# A workspace that has never relaunched since 1b landed will MISMATCH —
# that is the expected, honest signal that it needs a relaunch, not a
# false pass. Exit non-zero if any agent diverges.

set -uo pipefail

ROOT="${CLAUDE_BUS_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# Resolve to the real repo root even when run from a workspace copy.
case "$ROOT" in
  */.workspaces/*) ROOT="${ROOT%%/.workspaces/*}" ;;
esac
cd "$ROOT"

fail=0
ck() {  # ck RC LABEL
  if [ "$1" = 0 ]; then echo "    ok: $2"; else
    echo "    MISMATCH: $2"; fail=1; fi
}

# Compare a workspace file against a main-tracked path. Diff the
# `jj file show` stream DIRECTLY via process substitution — never buffer
# it in $(...), which strips trailing newlines and yields a spurious
# one-line "No newline at end of file" mismatch against the on-disk file.
# diff follows symlinks, so a pre-1b symlinked config compares by content.
same_as_main() {  # same_as_main MAIN_REL WS_FILE
  diff <(jj --ignore-working-copy file show -r main "$1" 2>/dev/null) \
       "$2" >/dev/null 2>&1
}

if ! jj --ignore-working-copy file show -r main \
      settings/claude-settings.json >/dev/null 2>&1; then
  echo "could not read main:settings/claude-settings.json — abort"; exit 1
fi

shopt -s nullglob
ws_count=0
for ws in "$ROOT"/.workspaces/*/; do
  agent="$(basename "$ws")"
  [ -d "$ws" ] || continue
  ws_count=$((ws_count + 1))
  echo "== $agent =="

  # 1) .claude/settings.json materialized == main
  if [ -e "$ws/.claude/settings.json" ]; then
    same_as_main settings/claude-settings.json "$ws/.claude/settings.json"
    ck "$?" ".claude/settings.json == main"
  else
    echo "    PENDING: no .claude/settings.json yet (not relaunched since 1b)"
    fail=1
  fi

  # 2) each materialized hook == main:settings/hooks-shared/<same>
  if [ -d "$ws/settings/hooks" ]; then
    for h in "$ws"/settings/hooks/*.sh; do
      rel="settings/hooks-shared/$(basename "$h")"
      if ! jj --ignore-working-copy file show -r main "$rel" >/dev/null 2>&1; then
        echo "    EXTRA: $(basename "$h") not on main"; fail=1; continue
      fi
      same_as_main "$rel" "$h"
      ck "$?" "hooks/$(basename "$h") == main"
    done
  else
    echo "    PENDING: no settings/hooks/ yet (not relaunched since 1b)"
    fail=1
  fi
done

echo
if [ "$ws_count" = 0 ]; then
  echo "no workspaces found under $ROOT/.workspaces — nothing to verify"
  exit 1
fi
if [ "$fail" = 0 ]; then
  echo -e "\033[1mCONFIG == MAIN for all $ws_count workspace(s)\033[0m"
else
  echo -e "\033[1mSOME WORKSPACES DIVERGE (relaunch needed, or canonical drift)\033[0m"
fi
exit "$fail"
