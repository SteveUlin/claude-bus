#!/usr/bin/env bash
# statusline.sh — capture-and-delegate wrapper for the agents' statusLine.
#
# WHY: the per-model REAL context window (.context_window.context_window_size,
# 200k vs 1M) and the LIVE effort level (.effort.level, reflects mid-session
# /effort) exist ONLY in the statusline JSON piped here on stdin — NOT in the
# transcript, so the broker's token-scan can't see them and falls back to the
# CLAUDE_BUS_CTX_WINDOW fleet constant. This wrapper is the one place those
# numbers are authoritative + live. It writes them to $STATE/statusline/<agent>
# .json (a NEW file — never $STATE/status, which the broker owns) for the
# monitor to read the TRUTH: bar = tokens / real window, with 200k as a policy
# marker, plus an EFFORT column. See docs/monitor-truth.md.
#
# Capture is best-effort and MUST NOT break the render: any failure falls
# through to delegating the original JSON to sulin's renderer unchanged.
# Project-scoped, so sulin's own (non-bus) sessions are untouched.

input=$(cat)

# Best-effort capture — guarded so nothing here can abort the delegate below.
{
  if command -v jq >/dev/null 2>&1 \
      && [ -n "${CLAUDE_BUS_AGENT_ID:-}" ] \
      && [ -n "$input" ]; then
    # Durable state root — must match bus::stateRoot() (src/state_paths.h).
    dir="${CLAUDE_BUS_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/claude-bus}/statusline"
    mkdir -p "$dir"
    dest="$dir/$CLAUDE_BUS_AGENT_ID.json"
    tmp="$dest.tmp.$$"
    ts=$(date +%s%3N)
    if printf '%s' "$input" | jq -c \
        --arg agent "$CLAUDE_BUS_AGENT_ID" \
        --argjson ts "$ts" \
        '{
           agent: $agent,
           ts: $ts,
           model_id: (.model.id // ""),
           model_display: (.model.display_name // ""),
           context_window_size: (.context_window.context_window_size // 0),
           total_input_tokens: (.context_window.total_input_tokens // 0),
           used_percentage: (.context_window.used_percentage // 0),
           effort_level: (.effort.level // ""),
           exceeds_200k: (.exceeds_200k_tokens // false)
         }' > "$tmp" 2>/dev/null; then
      mv -f "$tmp" "$dest" 2>/dev/null
    else
      rm -f "$tmp" 2>/dev/null
    fi
  fi
} 2>/dev/null || true

# Delegate to sulin's renderer so the in-pane status bar is unchanged.
real="/home/sulin/.claude/statusline-command.sh"
if [ -x "$real" ]; then
  printf '%s' "$input" | "$real"
else
  # No renderer available — emit a minimal line rather than nothing.
  printf '%s' "$input" | jq -r '.model.display_name // "Claude"' 2>/dev/null || printf "Claude"
fi
