# A Better FOCUS Column

The dashboard's FOCUS column should answer "what is this agent doing
right now?" in 28 characters. Today's chain — UserPromptSubmit body →
`last_event:last_tool` → `booting` → `—` — answers the wrong question.
The prompt is what the agent was *asked*; the tool name is *how* they
work; neither is *what they're doing*.

## Options surveyed

**A. `bus focus set "..."` self-report.** New CLI verb; agent calls it
when entering a sub-task. Explicit and intentional, but drift-prone
(agents forget to update, never clear). Adds bus surface.

**B. TodoWrite / TaskUpdate hook captures `activeForm`.** Claude Code
already mandates an `activeForm` ("Running tests", "Refactoring eval.h")
on the in-progress task — present-continuous, imperative, agent-authored.
A `PostToolUse` hook matched on TodoWrite/TaskUpdate parses the payload,
finds the entry with `status: "in_progress"`, writes its `activeForm` to
`$STATE/focus/$CLAUDE_BUS_AGENT_ID.txt`. Empties the file when no task
is in progress. Free, no LLM cost, no new verb, no agent prompting.

**C. Small-model summarization.** Hook on `Stop` sends recent turns to
Haiku for a four-word focus. Accurate, but per-Stop LLM cost (thousands
of calls/day across the fleet), lags realtime, fails closed when the API
hiccups.

**D. Claude session metadata.** `customTitle` is just the agent name;
nothing in the transcript JSONL captures task granularity. Free but
empty.

**E. Hybrid (B + current chain).** Primary signal: in-progress
`activeForm`. Fallback: today's chain.

**Rejected without survey:** pane scrollback scraping (no structured
"current task" marker emerges from claude's TUI); tool-arg abstraction
(captures mechanism, not intent).

## Recommendation: E — hybrid, with B as primary

Why this beats the alternatives:

- **Signal density.** `activeForm` is a sentence the agent wrote about
  itself, in the imperative form sulin already wants. It's the highest
  signal-per-character source we can get without paying for an LLM.
- **Zero new abstractions.** Agents already use TodoWrite for their own
  work. The "discipline" we're piggybacking on is one they keep current
  for themselves, not for the monitor.
- **Graceful degradation.** Agents that don't use TodoWrite (or are
  mid-tool with no task tracked) fall through to today's chain. Nothing
  regresses.
- **Latency.** Hook fires per `PostToolUse(TodoWrite)`. The dashboard
  picks it up on the next 1 Hz tick. Real-time enough.
- **Cost.** Zero.

Risks and mitigations:

- *Agents abandon tasks without marking them completed.* Focus stales.
  Mitigation: the dashboard's `AGE` column already shows when the
  agent last did anything; a stale focus on a stale agent reads as
  stuck, which is correct.
- *Multiple in-progress tasks.* Pick the most recently updated. Or
  concatenate the first two with ` · ` separator.
- *Agents who never use TodoWrite.* Fallback chain handles them. Over
  time, surfacing FOCUS in the dashboard becomes its own nudge.

## Sketch (not for implementation)

- `settings/hooks/focus-write.sh` — bash hook. On `PostToolUse` matched
  on `TodoWrite` and `TaskUpdate`, jq the payload for the in-progress
  entry's `activeForm`, write to `$STATE/focus/$CLAUDE_BUS_AGENT_ID`.
  Empty payload → remove the file.
- `.claude/settings.json` — register the hook alongside the existing
  ones.
- `src/sub/sub_monitor.cpp` — new helper `focusFromFile(name)`. Insert
  it at the head of the FOCUS priority chain.
- No broker changes. No new bus verb. ~30 lines of bash + ~15 lines of
  C++ when we're ready to build.

## Out of scope

- A retroactive backfill from prior TodoWrite calls. Live capture is
  enough; the dashboard is about the present moment.
- Showing focus history per agent. The bus event log already records
  that.
