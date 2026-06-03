#!/usr/bin/env bash
# deploy-ordering-itest.sh — gap #15 regression: lock the invariant that
# EVERY hook/command script REFERENCED in settings/claude-settings.json is
# present to be MATERIALIZED into every workspace. A landed settings.json that
# references a script absent from settings/hooks-shared/ deploys a dangling
# `$CLAUDE_PROJECT_DIR/settings/hooks/<x>` ref — the hook (or statusLine) then
# fails on every agent. That is the bug that just BLANKED THE FLEET BAR: the
# statusLine command pointed at settings/hooks/statusline.sh before/without the
# script materializing.
#
# WHY THIS MATTERS (the deploy mechanism, bin/agent-launch): on every launch
# agent-launch materializes config from LANDED main into each workspace —
# claude-settings.json -> .claude/settings.json, and `git archive <main>
# settings/hooks-shared | tar -x --strip-components=2 -> settings/hooks/`. So a
# referenced script lands in a workspace IFF it exists under hooks-shared/ at
# that commit. settings.json and hooks-shared MUST stay consistent or a launch
# deploys a broken reference. This test makes that consistency a hard gate.
#
# Two checks: (1) AUTHORING — working-tree settings.json refs ⊆ working-tree
# hooks-shared (catches the bad edit BEFORE you land it); (2) DEPLOY — replay
# agent-launch's exact materialize FROM main and assert every ref from main's
# settings.json actually lands (catches a broken materialize path too, e.g. a
# strip-components/glob regression). No broker/zellij needed — pure static.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# The materialize reads the MAIN checkout's git dir (agent-launch's BUS_ROOT),
# not a per-agent workspace (jj workspaces have no usable .git). Strip the
# /.workspaces/<name> suffix to recover the main checkout; a no-op when ROOT
# already IS the main checkout.
BUS_ROOT="${ROOT%/.workspaces/*}"
GITDIR="$BUS_ROOT/.git"
fail=0
ck() { if [ "$1" = "$2" ]; then echo "  ok: $3"; else
  echo "  FAIL: $3 (got [$1] want [$2])"; fail=1; fi; }

# Extract every settings/hooks/<script> referenced anywhere in a settings.json
# blob (hook command strings AND the statusLine command). Basenames, unique.
refs_from() { grep -oE 'settings/hooks/[A-Za-z0-9_.-]+' | sed 's#.*/##' | sort -u; }

echo "1. AUTHORING invariant — every script referenced in the working-tree"
echo "   settings/claude-settings.json exists in settings/hooks-shared/"
missing_wt=""
while IFS= read -r s; do
  [ -z "$s" ] && continue
  if [ ! -f "$ROOT/settings/hooks-shared/$s" ]; then missing_wt="$missing_wt $s"; fi
done < <(refs_from < "$ROOT/settings/claude-settings.json")
ck "${missing_wt:-none}" "none" "all working-tree settings.json refs are present in hooks-shared/ (missing:${missing_wt:-none})"

echo "2. DEPLOY invariant — replay agent-launch's materialize from LANDED main"
echo "   and assert every ref in main's settings.json actually materializes"
main_rev="$(jj --ignore-working-copy -R "$ROOT" log --no-graph -r main -T 'commit_id' 2>/dev/null)"
if [ -z "$main_rev" ]; then
  echo "  (skip: could not resolve main rev)"; else
  matdir="$(mktemp -d /tmp/deploy-ordering.XXXXXX)"
  trap 'rm -rf "$matdir"' EXIT
  # EXACT command from bin/agent-launch (hooks materialization).
  if git --git-dir="$GITDIR" archive "$main_rev" settings/hooks-shared 2>/dev/null \
        | tar -x --strip-components=2 -C "$matdir" 2>/dev/null; then
    missing_dep=""
    while IFS= read -r s; do
      [ -z "$s" ] && continue
      if [ ! -f "$matdir/$s" ]; then missing_dep="$missing_dep $s"; fi
    done < <(git --git-dir="$GITDIR" show "$main_rev:settings/claude-settings.json" 2>/dev/null | refs_from)
    ck "${missing_dep:-none}" "none" "every ref in main's settings.json materialized (missing:${missing_dep:-none})"
  else
    echo "  FAIL: materialize (git archive | tar) failed"; fail=1
  fi
fi

echo "3. RUNTIME invariant (#15 fix) — agent-launch must rm -f .claude/settings.json"
echo "   BEFORE materializing it, so it's a FROZEN per-workspace copy (not a write"
echo "   THROUGH a legacy symlink → a live view that desyncs from frozen hooks/)"
AL="$ROOT/bin/agent-launch"
rm_line=$(grep -nE 'rm -f[^#]*\.claude/settings\.json' "$AL" 2>/dev/null | head -1 | cut -d: -f1)
show_line=$(grep -nE 'show .*:settings/claude-settings\.json' "$AL" 2>/dev/null | head -1 | cut -d: -f1)
if [ -n "$rm_line" ] && [ -n "$show_line" ] && [ "$rm_line" -lt "$show_line" ]; then
  ck "ok" "ok" "agent-launch rm -f's .claude/settings.json before the materialize write (line $rm_line < $show_line)"
else
  ck "rm=${rm_line:-none},show=${show_line:-none}" "ok" "agent-launch must rm -f .claude/settings.json before materializing it (#15 runtime fix; a revert reintroduces the live-symlink bar-blank)"
fi

echo
if [ "$fail" = 0 ]; then
  echo -e "\033[1mDEPLOY-ORDERING OK: settings.json refs ⊆ materialized hooks (no dangling deploy)\033[0m"
else
  echo -e "\033[1mDEPLOY-ORDERING FAILED: a settings.json reference would deploy unmaterialized — fleet-bar-blank risk\033[0m"
fi
exit "$fail"
