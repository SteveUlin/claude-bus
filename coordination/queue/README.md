# queue — a shared work-queue, dispatched by the broker

Agents pull tasks from a shared queue when idle. Producers enqueue work; the
broker assigns each task to an idle agent.

## What this pattern is

A `work-queue` topic holds tasks. The broker's **`DispatchActor`** (a Policy
actor that ships in `bin/bus`) watches the queue head and the readiness of every
agent, and assigns the head task to one idle agent — delivered as ordinary inbox
mail. The claim (consuming the task so it isn't re-assigned) is performed by the
broker, reusing the same cursor advance as `bus msg fetch`.

This is the first coordination pattern realized as a **Policy actor** rather than
a layer of per-agent hooks + scripts (M2; the actor contract lives in
`src/policy.h` and `src/dispatch_actor.h`).

## How a pattern activates

`coordination/README.md` left "how does a pattern activate" open. This pattern's
answer: **its broker-side IS the Policy actor, which already ships.** Activation
is one step — create the topic:

```
bus topic create work-queue --kind work-queue
```

That's it. The `DispatchActor` is already registered in the broker (it was inert
because no work-queue existed); now it assigns. **No loader, no `hooks/`, no
`agent-prompt.md`, no per-agent Stop-hook pulling the next item.** Agents need
zero coordination code — they receive tasks as ordinary inbox mail through the
delivery path they already use.

## Use it

```
# producer: add work
bus msg enqueue work-queue "summarize docs/broker-spec.md"
bus msg enqueue work-queue "review the open PR"

# the broker assigns each task to an idle agent's inbox automatically.
# inspect the queue:
bus msg peek work-queue            # remaining (unclaimed) tasks
```

## What it costs / MVP scope

- **One assignment per tick** to the first eligible idle agent (ready at the
  prompt, no human attached, not mid blocking-op, not already holding queued /
  in-flight work). Re-assignment is gated by the agent's own inbox state, so
  tasks spread across idle agents without piling on one.
- **Assign-and-forget**: if an agent dies mid-task, the task is not yet
  re-queued (a documented follow-on). Batch dispatch + load-balancing are also
  follow-ons — the MVP proves the pattern, not a scheduler.
- **Sole consumer**: the `DispatchActor` is the queue's only consumer; don't also
  `bus msg fetch` the same work-queue from an agent (two claim paths would race).

## Files

Only this README. There are intentionally no `hooks/`, `scripts/`, or `state/` —
the pattern is broker-side code that already ships. That absence *is* the point.
