#!/usr/bin/env bash
# statusline-write.sh — Claude Code statusline script.
#
# Double-duty: reads the statusline JSON on stdin, atomically writes
# $STATE/status/$CLAUDE_BUS_AGENT_ID.json (the file `bus deck` and the
# proposed monitor CTX% column read), and prints the status string
# Claude Code displays at the bottom of the prompt.
#
# Wired via .claude/settings.json's statusLine block. Fires after every
# assistant message, /compact, perm-mode change, and on the configured
# refresh interval. The statusline JSON is the only surface that
# exposes live per-agent context-window % — hooks don't see it. See
# docs/context-budget.md and docs/observability-research.md (Design A).

set -uo pipefail

NAME=${CLAUDE_BUS_AGENT_ID:-}
STATE=${CLAUDE_BUS_STATE:-/tmp/claude-bus}

payload=$(cat)
# Empty stdin: print a minimal status so Claude Code doesn't render a
# blank line, and exit. No file to write.
if [ -z "$payload" ]; then
    printf '%s\n' "${NAME:-claude}"
    exit 0
fi

# --- atomic file write -----------------------------------------------
# Skip when the agent id is unset (transient one-shots, raw `claude`
# runs outside the fleet). Project only the fields downstream readers
# care about, plus an ms-precision ts for staleness detection.
if [ -n "$NAME" ]; then
    dir="$STATE/status"
    mkdir -p "$dir" 2>/dev/null
    tmp="$dir/$NAME.json.tmp.$$"
    if printf '%s' "$payload" | jq --arg agent "$NAME" '{
        agent: $agent,
        ts: (now * 1000 | floor),
        model: .model,
        cwd: .cwd,
        workspace: .workspace,
        context_window: .context_window,
        exceeds_200k_tokens: .exceeds_200k_tokens,
        cost: .cost,
        effort: .effort,
        rate_limits: .rate_limits
    }' > "$tmp" 2>/dev/null; then
        mv -f "$tmp" "$dir/$NAME.json"
    else
        rm -f "$tmp" 2>/dev/null
    fi
fi

# --- statusline output -----------------------------------------------
# Format:  <agent> • <ctx%> • $<cost>
# Each field drops out gracefully when its source is missing, so the
# line is never empty.
pct=$(printf '%s' "$payload" | jq -r '.context_window.used_percentage // empty' 2>/dev/null)
cost=$(printf '%s' "$payload" | jq -r '.cost.total_cost_usd // empty' 2>/dev/null)

label=${NAME:-claude}
out=$label
if [ -n "$pct" ] && [ "$pct" != "null" ]; then
    # used_percentage arrives as a float; floor for display.
    pct_i=$(printf '%.0f' "$pct" 2>/dev/null)
    [ -n "$pct_i" ] && out="$out • ${pct_i}%"
fi
if [ -n "$cost" ] && [ "$cost" != "null" ]; then
    cost_f=$(printf '%.2f' "$cost" 2>/dev/null)
    [ -n "$cost_f" ] && out="$out • \$$cost_f"
fi

printf '%s\n' "$out"
exit 0
