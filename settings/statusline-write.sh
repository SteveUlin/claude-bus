#!/usr/bin/env bash
# statusline-write.sh — Claude Code statusline script.
#
# Double-duty: reads the statusline JSON on stdin, atomically writes
# $STATE/status/$CLAUDE_BUS_AGENT_ID.json (the file `bus deck` and the
# monitor CTX% column read), and prints the multi-line status string
# Claude Code displays at the bottom of the prompt.
#
# Wired via .claude/settings.json's statusLine block. Fires after every
# assistant message, /compact, perm-mode change, and on the configured
# refresh interval. The statusline JSON is the only surface that
# exposes live per-agent context-window % — hooks don't see it. See
# docs/context-budget.md and docs/observability-research.md (Design A).
#
# Output shape:
#   line 1: <agent> · <model> · <ctx%>/<window_size> · $<cost>
#   line 2: 5h <remaining>% (resets HH:MM) · 7d <remaining>% (resets Day)
# Line 2 only renders when rate_limits are present in the payload
# (Pro / Max subscribers, after the first API response of the session).

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

# --- helpers ---------------------------------------------------------
# 200000 -> "200k"; 1000000 -> "1M"; 128000 -> "128k". Falls back to
# the bare integer for sub-1k values.
fmt_size() {
    local n=$1
    if [ "$n" -ge 1000000 ] 2>/dev/null; then
        awk -v n="$n" 'BEGIN{ if(n%1000000==0) printf "%dM", n/1000000; else printf "%.1fM", n/1000000 }'
    elif [ "$n" -ge 1000 ] 2>/dev/null; then
        awk -v n="$n" 'BEGIN{ printf "%dk", n/1000 }'
    else
        printf '%d' "$n"
    fi
}

# --- statusline output -----------------------------------------------
label=${NAME:-claude}

model=$(printf '%s' "$payload"  | jq -r '.model.display_name // .model.id // empty'             2>/dev/null)
pct=$(printf '%s' "$payload"    | jq -r '.context_window.used_percentage // empty'               2>/dev/null)
size=$(printf '%s' "$payload"   | jq -r '.context_window.context_window_size // empty'           2>/dev/null)
cost=$(printf '%s' "$payload"   | jq -r '.cost.total_cost_usd // empty'                          2>/dev/null)
u5=$(printf '%s' "$payload"     | jq -r '.rate_limits.five_hour.used_percentage // empty'        2>/dev/null)
r5=$(printf '%s' "$payload"     | jq -r '.rate_limits.five_hour.resets_at // empty'              2>/dev/null)
u7=$(printf '%s' "$payload"     | jq -r '.rate_limits.seven_day.used_percentage // empty'        2>/dev/null)
r7=$(printf '%s' "$payload"     | jq -r '.rate_limits.seven_day.resets_at // empty'              2>/dev/null)

# Line 1.
line1=$label
[ -n "$model" ] && [ "$model" != "null" ] && line1="$line1 · $model"

if [ -n "$pct" ] && [ "$pct" != "null" ]; then
    pct_i=$(printf '%.0f' "$pct" 2>/dev/null)
    if [ -n "$pct_i" ]; then
        if [ -n "$size" ] && [ "$size" != "null" ] && [ "$size" -gt 0 ] 2>/dev/null; then
            line1="$line1 · ${pct_i}%/$(fmt_size "$size")"
        else
            line1="$line1 · ${pct_i}%"
        fi
    fi
fi

if [ -n "$cost" ] && [ "$cost" != "null" ]; then
    cost_f=$(printf '%.2f' "$cost" 2>/dev/null)
    [ -n "$cost_f" ] && line1="$line1 · \$$cost_f"
fi

# Line 2: rate-limit window remainders. Each window is independently
# optional (the seven_day field can be absent while five_hour is set,
# or both can be absent on non-Pro plans).
line2=""
if [ -n "$u5" ] && [ "$u5" != "null" ] && [ -n "$r5" ] && [ "$r5" != "null" ]; then
    rem5=$(awk -v u="$u5" 'BEGIN{ r=100-u; printf "%d", (r<0?0:r) }')
    time5=$(date -d "@$r5" +%H:%M 2>/dev/null)
    [ -n "$time5" ] && line2="5h ${rem5}% (resets $time5)"
fi
if [ -n "$u7" ] && [ "$u7" != "null" ] && [ -n "$r7" ] && [ "$r7" != "null" ]; then
    rem7=$(awk -v u="$u7" 'BEGIN{ r=100-u; printf "%d", (r<0?0:r) }')
    day7=$(date -d "@$r7" +%a 2>/dev/null)
    if [ -n "$day7" ]; then
        seg="7d ${rem7}% (resets $day7)"
        if [ -n "$line2" ]; then line2="$line2 · $seg"; else line2=$seg; fi
    fi
fi

printf '%s\n' "$line1"
[ -n "$line2" ] && printf '%s\n' "$line2"
exit 0
