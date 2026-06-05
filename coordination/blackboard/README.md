# blackboard — shared notes, broadcast by the broker

A shared topic all agents read; when someone posts, the broker notifies everyone
else that the board changed.

## What this pattern is

A `blackboard` topic holds posts (a shared, append-only, non-destructively-read
note board). The broker's **`BlackboardActor`** (a Policy actor that ships in
`bin/bus`) watches for new posts and fans out a notify to every live agent
**except the author**, delivered as ordinary inbox mail. Agents read the full
board with `bus msg peek` (which does not consume it).

This is the second coordination pattern realized as a **Policy actor**, proving
the substrate admits **fan-out** (1→N broadcast) as cleanly as the work-queue's
competing-consumer claim (1→1). See
[docs/blackboard-notify.md](../../docs/blackboard-notify.md) and
[docs/policy-actors.md](../../docs/policy-actors.md) §6. It is the *purest* actor
— stateless: its watermark is a loop-owned cursor.

## How a pattern activates

Same answer as the queue: **its broker-side IS the Policy actor, which already
ships.** One step — create the topic:

```
bus topic create notes --kind blackboard
```

The `BlackboardActor` is already registered (inert until a blackboard topic
exists). **No loader, no `hooks/`, no `agent-prompt.md`.**

## Use it

```
# any agent posts to the board
bus msg enqueue notes "decision: we ship Plan A"

# every other live agent gets a blackboard-notify in its inbox; read the board:
bus msg peek notes
```

## What it costs / MVP scope

- **Notify all live agents** (minus the author). Subscriber opt-in,
  rate-limiting noisy boards, and a "board changed, go read it" heads-up vs.
  full-body broadcast are follow-ons.
- **Consume-on-read**: a notify is best-effort. An agent offline when a post
  lands won't get a retro-notify — it reads the current board on arrival (the
  correct behavior for shared retained state).

## Files

Only this README — no `hooks/`, `scripts/`, or `state/`. The pattern is
broker-side code that already ships; that absence is the point.
