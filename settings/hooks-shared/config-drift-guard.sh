#!/usr/bin/env bash
# config-drift-guard.sh — PostToolUse. Periodic self-heal for materialized hook
# drift.
#
# agent-launch materializes settings/hooks from claude-bus's LANDED main exactly
# ONCE per launch. A long-lived agent that never crashes (a coordinator) never
# re-runs agent-launch, so if its launch caught a stale main_rev (e.g. a
# config-cut window) it silently drifts — hooks keep routing events to a stale
# state root (/tmp), the broker stops seeing the agent's events, derives it
# STUCK, and its inbox stalls. That is exactly the 2026-06-04 auri wedge, and it
# lands on the one agent we can least afford to lose. This guard re-materializes
# the workspace's hook scripts from CURRENT main, rate-limited, overwrite-in-
# place (no rm-rf gap — hook scripts are re-read per event, so the heal takes
# effect on the agent's next tool use without a relaunch).
#
# Runs on PostToolUse (after the tool, so it can never block or delay it) and
# ALWAYS exits 0. settings/hooks-shared is the canonical source; .claude/
# settings.json drift is NOT healed here (it is read once at session start, so
# only a relaunch refreshes it) — this guard covers the recurring failure mode,
# stale hook *bodies*.
set -uo pipefail

STATE="${CLAUDE_BUS_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/claude-bus}"
GUARD_INTERVAL_S=600                       # check at most once per 10 min/agent
agent="${CLAUDE_BUS_AGENT_ID:-unknown}"
bus_root="${CLAUDE_BUS_ROOT:-}"
ws="${CLAUDE_BUS_AGENT_WORKSPACE:-${CLAUDE_PROJECT_DIR:-$PWD}}"

# Need a known bus root and a materialized hooks dir to act on; otherwise no-op.
[ -n "$bus_root" ] && [ -d "$ws/settings/hooks" ] || exit 0

# Rate-limit: the common path is a stamp stat + arithmetic, nothing more.
stampdir="$STATE/config-guard"; stamp="$stampdir/$agent.checked"
mkdir -p "$stampdir" 2>/dev/null || true
if [ -f "$stamp" ]; then
    now=$(date +%s 2>/dev/null || echo 0)
    last=$(stat -c %Y "$stamp" 2>/dev/null || echo 0)
    [ $(( now - last )) -lt "$GUARD_INTERVAL_S" ] && exit 0
fi
: > "$stamp" 2>/dev/null || true           # stamp the ATTEMPT (don't hammer on failure)

# Resolve claude-bus's landed main the same way agent-launch does: staleness-
# and ref-lag-immune (skip the working-copy snapshot, read the commit object).
main_rev=$(jj --ignore-working-copy -R "$bus_root" log --no-graph -r main \
    -T 'commit_id' 2>/dev/null)
[ -n "$main_rev" ] || exit 0

# Materialize current main's hooks to a temp dir, then overwrite only the files
# that actually differ (cmp), so an in-sync agent does zero writes.
tmp=$(mktemp -d 2>/dev/null) || exit 0
trap 'rm -rf "$tmp"' EXIT
git --git-dir="$bus_root/.git" archive "$main_rev" settings/hooks-shared 2>/dev/null \
    | tar -x --strip-components=2 -C "$tmp" 2>/dev/null || exit 0

healed=""
for f in "$tmp"/*.sh; do
    [ -e "$f" ] || continue
    b=$(basename "$f"); dst="$ws/settings/hooks/$b"
    if ! cmp -s "$f" "$dst" 2>/dev/null; then
        cp "$f" "$dst" 2>/dev/null && chmod +x "$dst" 2>/dev/null && healed="$healed $b"
    fi
done

# Audit only when drift was actually corrected — to a SEPARATE log, never
# events.jsonl (which is the broker's state feed). Also surface on stderr so it
# shows in the pane.
if [ -n "$healed" ]; then
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "?")
    printf '%s main=%s healed:%s\n' "$ts" "${main_rev:0:12}" "$healed" \
        >> "$stampdir/$agent.healed.log" 2>/dev/null || true
    echo "config-drift-guard: re-materialized stale hooks from main ${main_rev:0:12}:$healed" >&2
fi
exit 0
