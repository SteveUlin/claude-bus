# Context budget — automated clearing triggers + monitor visibility

Author: elodin · For: comms / sulin · Status: proposal, no code yet.

`docs/clear-policy.md` answers "when should *a human or agent* run `/clear`?" That doc biases toward NOT clearing — caution makes sense when the cost of a wrongly-cleared mid-task is high.

This doc answers a different question: **what automated triggers should fire `/clear` or `/compact` aggressively, and how does the human see context usage at a glance?** Today there's no automation — agents fill their context until claude code auto-compacts on its own, at which point cost has already been paid and continuity is already partly lost. Sulin's ask is to get ahead of that with cheaper triggers, and to give the dashboard enough signal that the human can intervene before any trigger fires.

This doc proposes triggers + a 1-line monitor-column proposal. No code ships here.

## Where context-usage data lives today

(From `reference-claude-code-token-data-sources` in auto-memory.)

The only surface that exposes live per-agent context-window % is the **statusline JSON** piped to a statusline script after every assistant message, `/compact`, perm-mode change, etc. Fields we care about:

- `context_window.used_percentage` — primary signal
- `context_window.total_input_tokens` / `total_output_tokens` — for absolute thresholds
- `exceeds_200k_tokens` — claude-code's own auto-compact flag, true when context is approaching ceiling
- `cost.total_cost_usd` — cumulative session cost, useful as a secondary signal

Hooks expose none of this. The transcript JSONL has per-turn usage but tailing it is more expensive than reading the statusline snapshot.

The bus doesn't currently route this data anywhere. The proposal below assumes a small statusline sidecar that writes `$CLAUDE_BUS_STATE/status/<agent>.json` on each tick — that sidecar isn't built yet but the design has been sketched (see `docs/observability-research.md`); the triggers below are written against it.

## Trigger candidates

Four broad triggers, with tradeoffs:

### 1. Absolute / percentage token threshold

Fire `/compact` when `context_window.used_percentage` crosses a threshold (e.g. 70%); fire `/clear` when it crosses higher (e.g. 90%) — i.e., when compacting is no longer enough.

- **Pros:** precise, ties directly to the cost driver. Reproducible across agents. Catches the "long thread that nobody noticed was getting heavy" case.
- **Cons:** percentage alone doesn't know if the context is *useful*. A 70%-full context that's all relevant working state is exactly what we want; a 70%-full context that's stale tool output is waste. The trigger fires the same on both.
- **When it makes sense:** workers (kvothe, bast, elodin in worker mode). Their context is task-bounded, so a high % during a task means the task is genuinely large; a high % between tasks means stale crud.

### 2. Idle-time + post-task

Fire `/clear` when: `last event = Stop` (agent finished a turn) **AND** `idle for ≥ N minutes` **AND** `inbox-<self>` is empty **AND** the recent transcript shows a commit-landed signature.

- **Pros:** matches the "task-bounded worker" model in clear-policy. Conservative — the agent really is between tasks. Cheap to detect from existing events.jsonl + cursor depth, no statusline dependency.
- **Cons:** "commit-landed signature" is fuzzy. False negatives (agent finished but hasn't committed yet); false positives (committed mid-task, work still ongoing). Need a clearer "I'm done" signal than `Stop` alone provides.
- **When it makes sense:** workers, after the agent itself reports task completion through the bus (a `[bast] done: <task>` mail is a strong "clear me" signal).

### 3. Cache-TTL boundary

This isn't a clearing *trigger* — it's a clearing *cost adjustment*. Within the 5-min cache window, `/clear` is strictly more expensive (forces a prefix re-read). Outside the window, clear is cost-neutral. So: gate ANY automated trigger on `now − last_assistant_turn > 5 min`. Anything earlier should defer.

- **Pros:** prevents the "expensive useless clear" pattern from clear-policy §4. Costs nothing to enforce.
- **Cons:** none. This is a check, not a trigger.
- **When it makes sense:** always. Every automated trigger respects this floor.

### 4. Pre-emptive compact on approaching ceiling

Fire `/compact` when `exceeds_200k_tokens` becomes true OR `used_percentage ≥ 85%`. This is the "get ahead of claude code's own auto-compact" trigger.

- **Pros:** controlled compaction beats surprise compaction. The summary lands when *we* pick the moment, not when claude code panics. Cost-equivalent to claude code's auto-compact since both pay the LLM summarization call.
- **Cons:** can fire mid-task if the task is genuinely long. Then a mid-task compact + the agent loses the "shape" of what it was doing. Net negative when compared to letting the long task finish.
- **When it makes sense:** workers that are clearly between turns (Stop event recent, no PreToolUse pending). Skip during active tool chains.

## Recommendation — ship two, defer two

**Ship: (2) idle + post-task and (3) cache-TTL gate.** Together they cover the common "worker idle between tasks" case with explicit signals (Stop event, empty inbox, idle minutes, cache cold) — no need for the statusline sidecar to exist yet, and no risk of clearing mid-task. Implementation surface: ~30 lines in the broker, observing events.jsonl, occasionally enqueueing `/clear` to a `commands-<agent>` topic.

Concrete trigger rule:
```
For each agent in registry-with-role="worker":
  IF last_event == "Stop"
     AND idle_minutes >= 10
     AND inbox-<agent> empty
     AND in-flight empty for <agent>
     AND now - last_assistant_turn > 5 min   (cache-TTL gate)
     AND the agent has emitted "done" / a commit landed in the last 20 min
  THEN enqueue /clear to commands-<agent>
```

The "done" / commit gate is the soft floor — it prevents firing on agents that are just idle-waiting between tool runs of the same task.

**Defer: (1) percentage threshold and (4) pre-emptive compact.** Both need the statusline sidecar, which doesn't exist. Worth a follow-up after the sidecar lands. (4) is the higher-value of the two — auto-compact-control is what claude code itself does next.

Per-role overrides apply on top:
- **comms**: never auto-clear (per clear-policy §6). High continuity, the human gates manually.
- **primary**: never auto-clear. Theme-bounded, gates at major transitions.
- **workers**: auto-clear on (2) + (3) above. The doc's existing self-check list still runs as a sanity gate before any auto-clear fires.
- **transient one-shots**: irrelevant, they exit.

## Monitor column proposal (hand off to kvothe)

`bus monitor`'s table grows one column: `CTX%` showing each agent's `context_window.used_percentage` from the statusline sidecar's `$CLAUDE_BUS_STATE/status/<agent>.json`. Width: 4–5 chars (`87%`, `100%`, `--` when no data). Color thresholds: dim under 50%, yellow 50–80%, red ≥ 80%.

That's it — 1 column, 1 data source, 1 file the sidecar already writes. The full sidecar design is in `docs/observability-research.md`; the column is the rendering surface.

## What this doc does NOT cover

- The statusline sidecar implementation itself (sketched in observability-research, hasn't shipped).
- Cost-tracking ($USD column) — a related but separate request worth its own pass.
- `/compact` policy in the *user-facing* sense — clear-policy already discusses /clear vs /compact tradeoffs.
- Per-agent triggers tuned to that agent's role beyond worker/comms/primary. Could matter once `roles/{kvothe,bast,elodin}.md` exists (currently only `roles/comms.md`).
