# Why auri Showed GONE — Follow-up to Spawn Dedup (b60a954)

Investigated after comms reported `bus state` rendered auri as GONE
with a SessionStart as the last event, even though a claude tab was
open. The dedup fix already in `b60a954` addresses the upstream
symptom; this note records what actually happened so we know what to
test against.

## Findings

`bus spawn auri` was invoked 3× before the dedup fix landed. Sequence
recovered from `events.jsonl`:

```
15:35:08  SessionStart  pane 13  source=resume
15:35:13  SessionStart  pane 15  source=resume
15:35:17  SessionStart  pane 17  source=resume
...
15:38:44  SessionEnd    pane 17  reason=other
15:38:44  SessionEnd    pane 15  reason=other
...
15:44:20  UserPromptSubmit  pane 13  (survived)
```

All three spawns used the same session UUID
`9e998e10-4ecf-40b9-8932-c2fc9febf765` — `bin/agent-launch` caches one
UUID per agent name at `~/.cache/claude-bus/agents/<name>.session`,
and the 2nd + 3rd spawn read the same cache. Each ran
`claude --resume 9e998e10-…`. Three claude processes contended for
the same transcript; ~3.5 min later, two exited with `reason=other`
(SessionEnd). pane 13 survived.

## Why GONE specifically

`agent_status.cpp:260-262`:

```cpp
if (!pane_exists) {
    ax.process = ev == "SessionEnd" ? ProcessAxis::Ended : ProcessAxis::Gone;
}
```

GONE requires last_event != SessionEnd **and** pane_exists == false.
Comms saw GONE-with-SessionStart, meaning at observation time the
last event for auri was a SessionStart but no pane named `auri` was
visible to `paneState(name)`.

Likely mechanism: in the window between the duplicate-deaths and the
next pane query, zellij was reshuffling pane IDs. `bus pane-id auri`
matches the first pane named `auri` it finds; if that matched a dying
duplicate, the broker's per-tick paneState query saw pane_exists =
false even though the survivor existed.

Now (after the survivor produced more events), `bus state auri`
correctly reports IDLE.

## What the dedup fix gets right

b60a954 prevents the triple-spawn → eliminates the root cause. The
transient-GONE artefact disappears with it.

## Follow-up worth considering (not for this PR)

- The broker's process axis flips to GONE on the *first* tick where
  pane_exists is false. One missed query is enough. A 2-of-N debounce
  would absorb transient pane churn without delaying genuinely gone
  agents by much.
- `bin/agent-launch`'s per-name session cache is the underlying
  shape that made duplicate spawn lethal in the first place. Worth a
  separate think about whether spawn should mint a new UUID by
  default and only resume via explicit flag.

Both are sharpenings, not gating issues. The dedup fix is enough to
close the reported bug.
