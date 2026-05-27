---
description: Deep card on one agent (registry + state + recent activity)
argument-hint: <agent-name>
---

For the agent named in the first argument:

1. Run `bus introduce $1` for the registry card + live state.
2. Run `bus events --agent $1 --since 30m` for recent activity.
3. If you need live pane contents, dump-screen:
   `zellij action dump-screen --pane-id "$(bus pane-id $1)"`.

Summarize for sulin:

- Who they are (kind / role / project / workspace).
- What they've been doing in the last 30 minutes (1–2 sentences).
- Whether they look available for new work.

Stop at the summary. Don't draft a message unless sulin asks.
