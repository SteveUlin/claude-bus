#!/usr/bin/env bash
# ready-write.sh EVENT_NAME
#
# Readiness sentinel writer (agent-contract.md Slice 1b). On a turn boundary
# the agent is its OWN author of "I'm parked, ready for the next turn": this
# writes $STATE/ready/<agent>.json atomically. The broker READS it (src/ready.h,
# isAgentIdle) instead of forking `zellij dump-screen` to parse the footer for
# an editable INSERT prompt — the off-TTY / stop-pane-reading move.
#
# Wired on Stop (turn ended → idle), SessionStart (resume/clear → at prompt;
# the crux — Notification(idle_prompt) does NOT fire reliably on resume/clear,
# which is exactly why the footer read existed), and Notification (idle_prompt
# → waiting for input; re-stamps so a long-idle agent stays off the pane past
# the sentinel TTL).
#
# COMPACT CARVE-OUT: a post-/compact SessionStart is NOT an idle boundary — the
# agent is compacting, and its drained additionalContext is swallowed (the
# post-compaction strand inbox-drain.sh also skips). Writing an idle sentinel
# here would let the broker's idle-gate deliver into that void. So skip the
# write on source=compact; computeState still reports Compacting from the event.
#
# Best-effort: any failure must NOT break the agent's turn (exit 0 always). The
# broker's readyFresh fence (TTL + "not older than last event") makes a missing
# or stale sentinel self-correct — a crash needs no cleanup hook.

set -uo pipefail

EVENT="${1:-}"
NAME="${CLAUDE_BUS_AGENT_ID:-}"
[ -n "$NAME" ] || exit 0  # not a fleet agent

input=$(cat 2>/dev/null || true)

# Compact carve-out (see header).
if [ "$EVENT" = "SessionStart" ]; then
  case "$input" in
    *'"source":"compact"'*) exit 0 ;;
  esac
fi

# Durable state root — must match bus::stateRoot() (src/state_paths.h) and
# statusline.sh's expression.
dir="${CLAUDE_BUS_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/claude-bus}/ready"
{
  mkdir -p "$dir"
  dest="$dir/$NAME.json"
  tmp="$dest.tmp.$$"
  ts=$(date +%s%3N)

  # session_id is opaque, for Slice-2 identity; the idle-gate doesn't read it,
  # so empty is fine when jq is absent. boundary_seq is a Slice-2 concern (the
  # doorbell's "new boundary?" check) — written 0 here, unused by the idle-gate.
  sid=""
  if command -v jq >/dev/null 2>&1 && [ -n "$input" ]; then
    sid=$(printf '%s' "$input" | jq -r '.session_id // ""' 2>/dev/null || printf '')
  fi

  if command -v jq >/dev/null 2>&1; then
    jq -cn --arg sid "$sid" --argjson ts "$ts" \
      '{session_id: $sid, boundary_seq: 0, ts_ms: $ts}' > "$tmp" 2>/dev/null \
      && mv -f "$tmp" "$dest" 2>/dev/null \
      || rm -f "$tmp" 2>/dev/null
  else
    # jq-free fallback: sid is "" (no injection risk); ts is an integer.
    printf '{"session_id":"","boundary_seq":0,"ts_ms":%s}\n' "$ts" > "$tmp" 2>/dev/null \
      && mv -f "$tmp" "$dest" 2>/dev/null \
      || rm -f "$tmp" 2>/dev/null
  fi
} 2>/dev/null || true

exit 0
