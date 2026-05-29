#!/usr/bin/env bash
# agent-register.sh EVENT
#
# Writes or removes this agent's registry entry on SessionStart / SessionEnd.
# Other event names are no-ops, so the hook can safely be wired into any
# Claude Code event without harm.
#
# Reads env vars set by bin/agent-launch:
#   CLAUDE_BUS_AGENT_ID                    (required; no-op if unset)
#   CLAUDE_BUS_AGENT_KIND                  (optional, null if unset)
#   CLAUDE_BUS_AGENT_ROLE                  (optional)
#   CLAUDE_BUS_AGENT_SESSION               (optional)
#   CLAUDE_BUS_AGENT_PROJECT               (optional)
#   CLAUDE_BUS_AGENT_WORKSPACE             (optional)
#
# Entry lives at $CLAUDE_BUS_STATE/agents/<name>.json. Schema:
#   {name, kind, role, session, project, workspace, pane_id, started_at}
# Missing string fields become JSON null. pane_id resolves via
# `bus pane-id NAME` and may be null if zellij hasn't surfaced the pane
# yet (best-effort; consumers can re-resolve at read time).
set -euo pipefail

EVENT=${1:-unknown}
NAME=${CLAUDE_BUS_AGENT_ID:-}
STATE=${CLAUDE_BUS_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/claude-bus}
# CLAUDE_PROJECT_DIR is exported by claude-code into hook env; fall back
# to walking up from this script's own path so the hook still works
# when invoked outside the agent context (e.g. manual testing).
BUS_ROOT=${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)}

[ -z "$NAME" ] && exit 0

REG_DIR="$STATE/agents"
ENTRY="$REG_DIR/$NAME.json"

case "$EVENT" in
  SessionStart)
    mkdir -p "$REG_DIR"
    PANE=$("$BUS_ROOT/bin/bus" pane-id "$NAME" 2>/dev/null || true)
    STARTED=$(date +%s)
    jq -n \
        --arg name      "$NAME" \
        --arg kind      "${CLAUDE_BUS_AGENT_KIND:-}" \
        --arg role      "${CLAUDE_BUS_AGENT_ROLE:-}" \
        --arg session   "${CLAUDE_BUS_AGENT_SESSION:-}" \
        --arg project   "${CLAUDE_BUS_AGENT_PROJECT:-}" \
        --arg workspace "${CLAUDE_BUS_AGENT_WORKSPACE:-}" \
        --arg pane      "$PANE" \
        --argjson started "$STARTED" \
        '{
            name:       $name,
            kind:       (if $kind      == "" then null else $kind      end),
            role:       (if $role      == "" then null else $role      end),
            session:    (if $session   == "" then null else $session   end),
            project:    (if $project   == "" then null else $project   end),
            workspace:  (if $workspace == "" then null else $workspace end),
            pane_id:    (if $pane      == "" then null else $pane      end),
            started_at: $started
        }' > "$ENTRY"
    ;;
  SessionEnd)
    rm -f "$ENTRY"
    ;;
esac
