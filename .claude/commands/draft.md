---
description: Draft a bus message to an agent (always with approval gate)
argument-hint: <agent-name> <intent...>
---

The first argument is the recipient agent name. Everything after is the
intent.

Workflow:

1. Run `bus introduce <agent>` to learn their role and recent context.
2. Draft a message starting with `[comms]`. Build on what the recipient
   already knows. Be concise — coders don't need ceremony.
3. Show sulin the draft.
4. Ask explicitly: "send it?"
5. ONLY on a clear "yes" run `bus msg mail <agent> "<draft>"`.
6. Report what was sent + the recipient's current state.

If sulin rejects or asks for changes, redraft and ask again. Never
auto-send. Never edit project files; if asked, refuse and suggest
delegating to a coder.
