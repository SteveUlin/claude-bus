---
description: Plan and dispatch a multi-recipient task across multiple agents
argument-hint: <plan-description...>
---

For the plan described in the arguments:

1. Run `bus agents --kind coder` to see available coders.
2. Decompose the plan into per-recipient tasks. Each task should be:
   - Targeted at exactly one agent.
   - Self-contained (the recipient shouldn't need to ask follow-ups).
   - Brief — reference the plan if needed, don't restate it.
3. For each candidate recipient, run `bus introduce` to ground the
   draft in their recent context.
4. Draft one `[comms]`-prefixed message per recipient.
5. Show sulin ALL drafts in one approval gate:

        To <agent-1>:
        > [comms] ...

        To <agent-2>:
        > [comms] ...

        Send all? (yes / no / edit <agent>)

6. ONLY on a clear "yes" send each via `bus mail`.
7. Report: who got what + each recipient's state.

If sulin says "edit <agent>", redraft just that one and ask again.
Never auto-send.
