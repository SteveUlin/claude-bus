# Delivery alternatives — push vs Monitor vs /loop vs hybrid

Author: elodin · For: auri / sulin · Status: proposal, no code yet.

Today's broker delivers mail by typing into the recipient agent's TTY: dispatch reads pane state, the broker's `sendToPaneSafe` writes the formatted record into the claude TUI's input buffer, presses Enter, the model wakes on a fresh UserPromptSubmit, broker observes the UPS event and acks the record. That path is simple in concept but has been the source of every delivery bug we've debugged this session: focus contention, mid-stream drops, `detectMode` brittleness, the wedge-from-slow-zellij, `mode=unknown` false-positive defers. Each fix has narrowed the failure mode without dismantling the underlying physical mechanism (typing into a shared TTY).

auri's task asks whether a pull-shaped delivery would be a cleaner long-term path. This doc evaluates four shapes — today's pure push plus three pull-or-hybrid alternatives — and recommends one.

## The four shapes

### A — Push (today's baseline)

`dispatchAgentInbox` → `deliverInline` → `sendToPaneSafe` → zellij `write-chars` + `Enter`. Broker observes the recipient's UserPromptSubmit in `events.jsonl` and acks.

- **Pros**: sub-second latency (one 250 ms tick + a couple of zellij subprocess calls). Single data model: one record, one delivery, one ack. The model's reaction to the new prompt is indistinguishable from a human-typed prompt — every claude code feature works.
- **Cons**: every failure mode comes from "typing into a shared TTY":
  - Focus contention with the human.
  - Mid-stream drops when injected during a tool chain (the documented "mid-stream silent dropped turn" memory).
  - `detectMode` and `paneState` brittleness across claude-TUI versions (just fixed for one variant; the next layout shift will surface another).
  - `sendToPaneSafe` defers on `mode=unknown` / `LOCKED` / scrolled panes, which lengthens the latency tail. Some of those defers are correct; some are false positives.

### B — Monitor watching a per-agent inbox file

Broker writes each delivery payload as one line to `$STATE/inbox/<agent>.log`. The recipient agent runs Claude Code's `Monitor` tool against `tail -F $STATE/inbox/$CLAUDE_BUS_AGENT_ID.log` at SessionStart. Each new line arrives as a queued harness notification — the same delivery path /loop output and AskUserQuestion responses use. The agent sees it on its next natural turn boundary; no TTY write involved.

- **Pros**: bypasses *every* TTY-shaped failure. No focus contention. No mid-stream interleave. `detectMode` becomes irrelevant. The harness already queues notifications across mid-tool-call boundaries.
- **Cons**:
  - Agent-side bootstrap: the recipient has to *invoke Monitor* before delivery works. Role prompt can instruct it, but boot is not zero-effort and a missed invocation means silent black-hole until a tool-call observes the gap.
  - Ack model changes: broker writes the line → that's delivery. Ack via the recipient's next UPS still works but the timing is fuzzier (the agent processes notifications at unpredictable points).
  - File rotation: the per-agent log grows unbounded; needs trimming policy.
  - One Monitor per agent — if the agent invokes it twice, double-delivery.
  - Monitor is a Claude Code tool: behaviour can shift across versions, same risk class we just exited on `detectMode`.

### C — /loop polling

Each agent self-schedules `/loop NN bus msg fetch inbox-<self>` at SessionStart. Every NN seconds the slash fires, the bash inside fetches any pending records, the model processes whatever the fetch returned.

- **Pros**: no broker→agent direct writes ever. Code path is what today's `bus msg fetch` already does. No new infrastructure. /loop is a documented Claude Code skill with a clean schedule semantic.
- **Cons**:
  - Latency = the poll interval. 30 s interval = up to 30 s mail delay; 1 min interval = up to 1 min. Always worse than push.
  - Per-tick cost: each fire is a full claude-code turn, even when mail is empty. Inside the prompt-cache window (5 min default TTL), reads are cheap; outside it, every fire pays the full prefix.
  - The /loop interval interacts with cache TTL: pick > 5 min and every fire is cache-cold, multiplying cost. Pick < 5 min and cache stays warm but the agent never gets a long quiet period.
  - Same agent-side bootstrap requirement as (B): a missed invocation means silent black-hole.

### D — Hybrid (push happy path + /loop fallback)

Broker pushes as today (sub-second when the pane is ready). Agents run `/loop NN bus msg fetch inbox-<self>` as a safety net. Push handles ~95% of cases; when `sendToPaneSafe` defers, the record sits in the topic log, and the next /loop tick fetches it instead.

To avoid double-delivery: `bus msg fetch` already advances the cursor on consume; today's push also advances on ack. Need one rule — "if a record is in-flight, fetch skips it" — so the fast path doesn't race the slow path. ~10 LOC in the fetch handler.

- **Pros**: keeps push's low-latency happy path. Adds robustness to every push failure class without re-engineering the delivery mechanism. Each path is small and well-understood — the hybrid is the union, not the intersection.
- **Cons**: two delivery mechanisms to maintain. The fetch-skips-in-flight rule is a real coordination point. Operational stories double (a stuck record could be on the broker's side OR the agent's polling side; need to diagnose which).

## Recommendation — ship D (hybrid)

The hybrid is the lowest-risk path that addresses auri's underlying concern (push is fragile) without throwing away the latency win that push delivers when it works.

Reasoning:
- Push's failure modes are real but **localized**: each one I've fixed this session was a narrow bug, not a fundamental design break. The architecture is sound; the implementation has rough edges. Tracking down the next `detectMode` variant is cheap compared to migrating delivery semantics.
- Pure (B) and (C) require *every* agent to bootstrap their receiver correctly. We don't control the agent's first-turn behaviour beyond what the role prompt encodes. A silent black-hole on a misconfigured boot is worse than a noisy retry on push.
- The hybrid degrades gracefully: push failures become latency increases, not data loss. The N-second `/loop` interval is a worst-case bound on staleness, not the typical latency.
- Implementation surface is small: ~10 LOC for the fetch-skips-in-flight rule, plus a single line in each agent's role prompt to invoke `/loop`. No new wire format, no new RPC, no broker restart required to gain the fallback.

If hybrid is later judged insufficient (e.g., the next `detectMode` variant lands and the push failure rate climbs), migrate to (B) with the role-prompt bootstrap as a separate cutover. The hybrid is forward-compatible: the topic log + fetch path is shared with the file-tail design.

## What'd change in src/

A modest pass:

- **`src/broker.cpp`** (fetch handler, ~10 LOC): when fetching from `inbox-<agent>` and the head record is currently in `in_flight_` for that agent, skip it and return the next non-in-flight record (or empty if none).
- **`roles/*.md`** (~1 line each): every agent's role prompt gets a one-liner: "On SessionStart, run `/loop 30s bus msg fetch inbox-<self>` to backstop direct delivery."
- **`src/sub/sub_consume.cpp`** (`subFetch`, optional): a `--no-in-flight` flag for explicitness, even though the broker-side rule is enough. Surface so callers can opt out.

Total estimated LOC: ~20-30 in the broker, ~5 in the CLI, ~N short role-prompt lines (where N = active agents).

## Out of scope

- File-rotation policy for `$STATE/inbox/<agent>.log` (Monitor's path) — not relevant in the hybrid recommendation. Revisit if we migrate to (B).
- The "broker writes to a sentinel file" half of (B). The hybrid path uses the existing topic log; no new write surface.
- Pure-pull migration. Belongs to a separate doc if/when the hybrid's push side proves unsalvageable.
- Cache-cost math under various `/loop` intervals — covered in `docs/clear-policy.md` and `docs/context-budget.md`; the same 5-min TTL economics apply.
