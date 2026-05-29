#!/usr/bin/env bash
# focus-write.sh — PostToolUse hook that maintains a per-agent FOCUS file.
#
# Writes the in-progress task's activeForm (or subject as fallback) to
# $STATE/focus/$CLAUDE_BUS_AGENT_ID. The bus monitor reads this as the
# first priority in its FOCUS column (see docs/monitor-focus.md).
#
# Three trigger surfaces:
#
#   TodoWrite — full list of todos arrives in tool_input.todos. Find the
#               entry with status:"in_progress"; write its activeForm.
#               If no in-progress entry, clear the focus file.
#
#   TaskCreate — single task created. tool_response.task carries id +
#                activeForm + subject + status. Cache the title under
#                $STATE/focus/$AGENT.tasks/<id> for later resolution.
#                If the task is created already in_progress, also set
#                the focus file.
#
#   TaskUpdate — single task changed. tool_input.taskId tells us which.
#                On status → in_progress, resolve the cached title and
#                write it to the focus file. On completed/deleted, drop
#                the cache entry and clear the focus file if it pointed
#                at this task.
#
# Everything not in those three tool names is a no-op so the hook can
# safely be wired under matcher:"" alongside log-event.sh.

set -uo pipefail

NAME=${CLAUDE_BUS_AGENT_ID:-}
[ -z "$NAME" ] && exit 0
STATE=${CLAUDE_BUS_STATE:-/tmp/claude-bus}
FOCUS_FILE="$STATE/focus/$NAME"
CACHE_DIR="$STATE/focus/$NAME.tasks"
mkdir -p "$STATE/focus" "$CACHE_DIR" 2>/dev/null

payload=$(cat)
tool=$(printf '%s' "$payload" | jq -r '.tool_name // empty' 2>/dev/null)

write_focus() {
    local text=$1
    [ -z "$text" ] || [ "$text" = "null" ] && return 0
    printf '%s\n' "$text" > "$FOCUS_FILE"
}

case "$tool" in
TodoWrite)
    in_prog=$(printf '%s' "$payload" | jq -r '
        (.tool_input.todos // [])
        | map(select(.status == "in_progress"))
        | first
        | (.activeForm // .subject // empty)
    ' 2>/dev/null)
    if [ -n "$in_prog" ] && [ "$in_prog" != "null" ]; then
        write_focus "$in_prog"
    else
        rm -f "$FOCUS_FILE"
    fi
    ;;
TaskCreate)
    tid=$(printf '%s' "$payload" | jq -r '.tool_response.task.id // empty' 2>/dev/null)
    [ -z "$tid" ] && exit 0
    title=$(printf '%s' "$payload" | jq -r '
        .tool_response.task.activeForm // .tool_response.task.subject // empty
    ' 2>/dev/null)
    [ -z "$title" ] || [ "$title" = "null" ] && exit 0
    printf '%s\n' "$title" > "$CACHE_DIR/$tid"
    status=$(printf '%s' "$payload" | jq -r '.tool_response.task.status // empty' 2>/dev/null)
    if [ "$status" = "in_progress" ]; then
        write_focus "$title"
    fi
    ;;
TaskUpdate)
    tid=$(printf '%s' "$payload" | jq -r '.tool_input.taskId // empty' 2>/dev/null)
    [ -z "$tid" ] && exit 0
    to=$(printf '%s' "$payload" | jq -r '
        .tool_response.statusChange.to // .tool_input.status // empty
    ' 2>/dev/null)
    case "$to" in
    in_progress)
        [ -f "$CACHE_DIR/$tid" ] && cp "$CACHE_DIR/$tid" "$FOCUS_FILE"
        ;;
    completed|deleted)
        # If the focused task was this one, clear it. The cache entry
        # gets dropped either way — completed tasks don't reappear.
        if [ -f "$CACHE_DIR/$tid" ] && [ -f "$FOCUS_FILE" ]; then
            cmp -s "$CACHE_DIR/$tid" "$FOCUS_FILE" && rm -f "$FOCUS_FILE"
        fi
        rm -f "$CACHE_DIR/$tid"
        ;;
    esac
    ;;
esac

exit 0
