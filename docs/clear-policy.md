# /clear policy for bus agents

`/clear` resets a Claude Code session's conversation context. Used wrong, it costs more than it saves and drops in-flight understanding the agent never wrote down. Used right, it sharpens a fresh task and trims pollution from a finished one. This doc proposes a policy that captures the difference.

This is a **proposal, not a mandate**. The rules below are guidelines for agents and humans; enforcement is discussed at the bottom.

## What `/clear` actually does

`/clear` truncates the model's conversation history for the current session. The next prompt arrives as if the agent just booted — system prompt, role file, and the new turn, nothing else. The session id and on-disk transcript are preserved (claude code rotates the transcript file), so external observers can still trace what happened; the *model* just doesn't see the prior turns.

What `/clear` is **not**: it isn't a process restart, it doesn't reload `CLAUDE.md` from disk in any privileged way (CLAUDE.md is part of the system prompt anyway, re-read each turn), and it doesn't touch any state outside the session.

## What survives `/clear`

Everything the bus relies on. The list is short and worth memorizing:

- **Git / jj history.** Commits are durable. So is the working copy.
- **Project memory.** Files under `~/.claude/projects/<repo>/memory/` are re-read on every turn; their content survives `/clear` by definition.
- **`CLAUDE.md`, role files, the system prompt.** Re-read on every turn.
- **Topic logs and cursors.** `/tmp/claude-bus/topics/*.log` and the per-consumer cursor files are owned by the broker, not the session.
- **Pending inbox.** Any messages waiting on `inbox-<self>` will still be there after `/clear`.
- **Pane state, presence files, broker in-flight tracker.** All external to the session.

## What does NOT survive

Anything the model held only in conversation context. The danger list:

- **In-flight reasoning.** A half-formed plan, a "I noticed X but haven't acted on it yet" observation, a hypothesis being tested.
- **Decisions not yet written down.** Choices made in dialogue but not committed to memory, a task, a plan file, or a commit.
- **Context about messages already processed.** If the agent received mail at 14:02, acted on it, and didn't write the conclusion anywhere — `/clear` drops the conclusion.
- **The shape of an ongoing collaboration.** "I'm waiting on bast's reply before I commit" is context that needs to be in a task or an explicit handoff, not just in the conversation.

**The rule:** if it isn't in a commit, a memory file, a task, or a queued message, `/clear` deletes it.

## When to `/clear`

The good cases share a structure: **the agent has finished one thing and is about to start another genuinely unrelated thing.**

1. After committing a feature and before starting a wholly unrelated task. The two share no useful context.
2. After a long debugging session that produced a fix. The fix is in the commit; the breadcrumbs that led there pollute new work.
3. Between role-bounded jobs in a worker (kvothe, bast, denna) — task-bounded agents *should* clear between tasks. That's their natural rhythm.
4. When context has grown long enough that auto-compaction is imminent. `/clear` is cheaper than waiting for the model to summarize-and-restart implicitly.

## When NOT to `/clear`

The traps share a structure too: **the agent thinks the task is over, but the context still matters.**

1. **Mid-task.** Obvious but easy to violate. Tool calls in flight, an edit not yet committed, a test not yet verified — keep going.
2. **With pending inbox.** `bus state <self>` shows `HAS_MAIL`? Process the queue first. Clearing first means the agent processes those messages without the context they were written against.
3. **Within the prompt-cache window** (see math below). `/clear` 4 minutes after the last turn forces a full cache-miss prefix re-read on the next prompt; waiting another minute and clearing then is free.
4. **Before queued work for which context matters.** "I'm about to receive a task description" doesn't help if the description references "the bug we discussed earlier" and the agent has cleared that bug.
5. **In a long-running collaboration thread.** comms tracking a multi-turn negotiation, primary holding the shape of a planning phase — these are the cases continuity dominates.

## Prompt-cache math (why too-eager `/clear` is *worse*)

Anthropic's prompt cache has a 5-minute TTL, refreshed on hit. The cached prefix is whatever the API marked with `cache_control` — for claude-bus agents, this is the system prompt + role file + early memory, the bulk of the per-turn token cost.

Three regimes:

| Time since last turn | Cache state | `/clear` effect on next-prompt cost |
|---|---|---|
| < 5 min | warm | **strictly worse** — clearing forces a full prefix re-read; continuing reuses the cached prefix |
| 5 min – ∞ | cold | neutral — the prefix would be re-read regardless |
| Mid-turn / mid-tool | warm | catastrophic if you actually clear; never do this |

The trap: agents (and humans) tend to clear when they "feel done." Feeling-done often arrives within the cache window. **Wait the 5 minutes, then clear if you still want to** — the cost of waiting is zero, the savings are real.

A second-order effect: if the agent expects to be re-invoked within minutes (e.g., during an active coordination thread), even a *cold* `/clear` becomes worse because the next prompt re-pays the prefix and then *immediately re-warms* it for the response, doubling effective cache traffic. Don't clear when you expect imminent re-engagement.

## Per-role differences

Roles in claude-bus have different temporal shapes. The policy should follow.

- **comms.** High continuity. Acts as a relay across many conversation threads, holds shape of who's working on what, gates message dispatch. `/clear` here drops cross-thread state nothing else captures. Recommendation: **never auto-clear; only on explicit human direction at clear inflection points** (e.g., end of a multi-day initiative).
- **primary.** Long-horizon orchestrator. Theme-bounded rather than task-bounded. Clear at boundaries between major themes (planning phase → implementation phase, one feature → another). Recommendation: **clear deliberately at theme transitions**, perhaps weekly during sustained work.
- **workers** (kvothe, bast, denna, elodin in worker mode). Task-bounded. Each unit of work is "investigate X, write Y, commit, hand off." Clear between units. Recommendation: **clear after each commit-and-handoff**, unless the next task explicitly inherits context (rare).
- **transient agents** (any one-shot dispatched via `bus dispatch` or similar). Don't bother clearing — they exit anyway.

A complementary rule: **whoever holds the most fragile continuity should clear the least**. comms > primary > workers, by design.

## Enforcement — three flavors

How does the policy actually get followed? Options span automation-vs-judgment.

1. **Broker auto-clear.** The broker watches events.jsonl, detects "last meaningful work N minutes ago + inbox empty + no in-flight," and dispatches `/clear` to the agent.
   - Pros: consistent, no human attention required.
   - Cons: the "is this agent done?" heuristic is hard. False positives clear mid-task. The 5-minute cache window adds another threshold. **Strongly not recommended** at current maturity.

2. **Manual.** Human runs `bus msg slash <agent> /clear` when they judge it. Backed by occasional dashboard prompts ("kvothe IDLE 20 min, last 3 commits unrelated — clear?").
   - Pros: human judgment is the most accurate.
   - Cons: requires attention, easy to forget.
   - Recommendation: **the baseline.**

3. **Self-discipline.** Agents decide. Reinforced via role prompt: "after committing a task and handing off, run `/clear` unless the next thing genuinely needs your context." The agent has the most information about whether its thread is done.
   - Pros: scales with the agent count.
   - Cons: agents have a known bias toward continuity (it's cheap for them inside a turn); the rule has to be written firmly.
   - Recommendation: **layered on top of manual** — agents propose `/clear` to themselves at boundaries, sulin overrides as needed.

The proposed combination is **(2) + (3)**: humans clear on judgment, agents self-clear at obvious boundaries, the broker stays out of it.

## Concrete checklist (for agents)

Before running `/clear` on yourself, verify:

- [ ] No tool calls in flight (`bus inflight` shows nothing for me).
- [ ] No pending messages on `inbox-<self>` (`bus state <self>` is `IDLE`, not `HAS_MAIL`).
- [ ] My last commit captures the work I just did. Open questions are in a task or memory.
- [ ] More than 5 minutes since my last turn, OR I don't expect to be re-engaged in the next 5 minutes.
- [ ] The next thing on my plate doesn't reference "the X we just discussed" or similar.

If any answer is "no," don't clear. If unsure, don't clear.

## `/clear` vs `/compact`

Two related commands worth distinguishing:

- **`/clear`** drops conversation history entirely. Next prompt starts fresh.
- **`/compact`** asks the model to summarize the conversation so far, then continues with the summary as context.

`/compact` is what claude code does automatically when the context window fills. It preserves *what the agent knows* in a lossy compressed form. `/clear` doesn't — it discards everything not external to the session.

When to prefer `/compact`:

- Mid-task and context is filling up.
- The conversation has a useful trajectory you'd lose entirely with `/clear`.
- You want to keep going on the same thing.

When to prefer `/clear`:

- The current task is done and the next thing is genuinely unrelated.
- The accumulated context is mostly noise (long debug breadcrumbs, dead-end explorations).
- You want a clean slate, not a compressed one.

Both pay a prompt-cache cost; `/compact` additionally pays an LLM-summarization cost. **`/clear` is the cheaper of the two when you can use it.** `/compact` is the fallback when you'd lose too much context.

## Worked examples

These ground the rules above.

### Worker after committing a feature

kvothe just landed `phase 5: typed pubsub topics`, the test suite passes, the commit is pushed. kvothe's next task in the queue is `phase 6: blackboard cursor semantics` — same project, totally different code. 

Verdict: **`/clear` is appropriate.** Wait until > 5 min since last turn (or right now if kvothe knows the cache is already cold), then clear. Phase 6 starts with a fresh head, no phase-5 breadcrumbs polluting the analysis.

### Worker mid-debugging-session

bast has been chasing a flaky test for an hour. The test passes locally, fails in CI. bast has half-formed hypotheses ("maybe it's a tempdir collision"), several diagnostic scripts in flight, no commits yet.

Verdict: **never clear here.** The half-formed hypotheses are the work product so far. Clearing forces bast to re-discover them. If context is getting tight, `/compact` instead — the model's summary will preserve the working hypothesis. Better still: write the hypothesis into a memory file or task and *then* take a break.

### comms during a multi-agent coordination thread

comms is tracking three concurrent investigations (kvothe on dispatch race, bast on agent-register, denna on notification cleanup). Mid-thread, comms has each agent's progress in conversation context.

Verdict: **never clear.** This is exactly what high-continuity means. Even at task boundaries, comms typically holds a wider thread than any individual worker. Clear only at the end of a multi-day initiative, on explicit human direction.

### primary after a planning phase

primary spent two days mapping out a refactor with sulin. The plan is now in `~/.claude/plans/refactor-X.md`, broken into tasks, dispatched to workers. The implementation phase doesn't need the planning conversation — it needs the plan file and the task list, both of which are on disk.

Verdict: **`/clear` is appropriate** at the planning → implementation transition. Primary re-enters the implementation phase reading the plan file fresh, without two days of "what about Y?" back-and-forth weighing it down.

## Open questions

These the policy doesn't answer yet:

- Should `/clear` events emit to events.jsonl? Currently `/compact` does (`SessionStart source=compact`); `/clear` is silent. The bus would benefit from a marker for cross-agent debugging.
- Should the broker auto-clear *transient agents* on exit, separately from the rest? Probably overkill — they exit anyway.
- Is there a useful "/clear but keep the last N turns" variant? Claude Code doesn't expose one today; if it did, it would change the policy materially.

## Summary

`/clear` is a cheap operation that becomes expensive when used inside the cache window or mid-task. The cost story is asymmetric: clearing too early costs prompt-cache money and lost context; clearing too late costs only minor context pollution. **Bias toward not clearing.**
