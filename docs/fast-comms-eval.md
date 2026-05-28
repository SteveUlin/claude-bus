# Fast-comms eval: a smaller comms with a planner peer

Author: elodin · For: comms / sulin · Status: proposal, no code yet.

sulin's intuition is that comms's work splits cleanly along two axes: the messaging surface (parse, route, format, relay — high frequency, low judgment) and the planning surface (who to dispatch, what prompt, how to triage replies — low frequency, high judgment). If that split is real, the fleet wastes Opus-tier inference on every relay turn when most of those turns just need to type a sentence into someone's inbox.

This doc evaluates whether to move comms onto a faster, cheaper model and offload planning to a separate peer. It cites four external prior-art frameworks that have already shipped some version of this split, runs the cache-window economics, names the failure modes, and ends with a recommendation.

## 1. What comms actually does today

I categorized comms's work by reading `roles/comms.md`, the slash commands (`.claude/commands/{dispatch,draft,peek,status}.md`), and recent thread traffic. Two clusters fall out.

**Messaging (high-frequency, low-judgment).** The mechanical relay surface:

- Receive a message on `inbox-comms`, identify the sender from the `[<sender>]` prefix.
- Pull peer context: `bus introduce`, optional `dump-screen`.
- Summarize for sulin in plain language; note what the sender wants next.
- After sulin approves, send via `bus msg mail` / `bus msg broadcast`.
- Acknowledge / surface delivery state.

This is well-defined transformation work. The prompt-template is short, the inputs are bounded (a few hundred bytes of bus context), and the output is a clear summary plus a `bash` call. Sonnet handles this comfortably; Haiku handles it adequately given the template structure.

**Planning (low-frequency, high-judgment).** The judgment surface:

- `/dispatch <plan>`: decompose a multi-agent task. Decide *who* receives *what*, in what order, with what prompt phrasing. Read 4–8 peers' registry cards and recent activity, then write 1–N tailored prompts.
- `/draft <agent> <intent>`: a single message but with real authoring — picking the right framing, the right precedents to reference, the right level of detail given the recipient's role.
- Triage: when an inbound reply is ambiguous ("kvothe says it's done but the test isn't passing"), comms reasons about whether to escalate, ask a follow-up, or merge into other context.
- Sequencing: when multiple threads are open, deciding which to advance vs. defer.

This is where smarter-model judgment pays off. Bad planning here is expensive — wrong recipient, vague prompt, missed dependency — because it cascades through other agents' work.

**Rough volume split.** Eyeballing recent traffic on the bus, the ratio looks like **roughly 80–85% messaging, 15–20% planning** by turn count. Messaging is dominant in time but planning is dominant in *value per turn*.

## 2. Where intelligence demand actually lives

Three of comms's responsibilities benefit from a bigger model; the rest don't:

| Surface | Frequency | Model demand |
|---|---|---|
| Parse + summarize inbound | high | low |
| Format outbound + bash call | high | low |
| `/peek`, `/status` (read-only inspection) | medium | low |
| `/draft` single-recipient prompt | medium | medium |
| `/dispatch` multi-recipient plan | low | high |
| Ambiguous-reply triage | low | medium-high |
| Multi-thread sequencing | low | medium-high |

This is the structural argument for the split: the rows at top fire many times per session and don't need an Opus-tier model to produce competent output. The rows at bottom fire rarely but each call really benefits from the extra reasoning.

**Worth flagging**: `roles/comms.md` already declares `model: claude-3-5-sonnet-20241022`, not an Opus tier. sulin's premise was that "comms is currently a big intelligence-tier model (Opus)" — there's a mismatch between sulin's premise and the role file. Could be an inherited assumption, could be that the role file is stale and a launcher is overriding the model. Worth confirming before any model swap so we're not optimizing against the wrong baseline.

## 3. Proposed splits

Comms named three options; I'll add two more and one to skip.

**(a) Haiku comms + dedicated Opus "planner" peer agent on the bus.** Comms runs Haiku for everything in the high-frequency messaging row. When it hits a `/dispatch`, ambiguous triage, or hard sequencing call, it mails a message to a `planner` peer (Opus) via the same bus, waits for the reply, and executes the recipe.

**(b) Sonnet comms, no planner peer.** Single-model design with a slightly smaller model than Opus. Sonnet 4 is the natural pick — it covers planning competently in our scale (4–6 agents, short threads).

**(c) Haiku comms + ephemeral Opus subagent via Task tool.** Comms spawns a one-shot subagent for each planning call. Subagent runs in-process under the same session, gets its own context, returns a structured recipe, tears down.

**(d) Haiku comms + cached planner instructions.** Skip the planner peer entirely and instead push the *planning prompt template* into comms's system prompt as a structured rubric. Haiku follows the rubric for `/dispatch`. This trades model intelligence for explicit decision procedures.

**Skip: full GroupChat-style supervisor with selector model.** AutoGen's `SelectorGroupChat` runs a per-turn LLM call to decide which agent speaks next ([AutoGen Group Chat](https://microsoft.github.io/autogen/stable//user-guide/core-user-guide/design-patterns/group-chat.html)). For our fleet of 4–6 agents where the human is the gating decision-maker, that's mostly overhead.

## 4. Prior art — how other frameworks split this

The "fast surface + smart planner" split isn't novel; four mainstream frameworks ship variants:

- **LangGraph supervisor pattern.** A supervisor agent receives the request, delegates subtasks to specialist workers, and synthesizes outputs. The pattern is most useful "when an agent has over 10 tools, tasks require multi-domain collaboration, or debugging becomes difficult." Cost reported at roughly 3× a single mega-agent because every supervisor turn is a full LLM call. ([LangGraph multi-agent guide](https://focused.io/lab/multi-agent-orchestration-in-langgraph-supervisor-vs-swarm-tradeoffs-and-architecture))

- **OpenAI Swarm / Agents SDK.** Each agent is "a container for instructions and functions"; a `handoff` is a function that returns another agent, transferring conversational control. Stateless, deliberately minimal. Swarm itself is deprecated in favor of the OpenAI Agents SDK (March 2025), which keeps the same conceptual model in a production-supported package. ([OpenAI Swarm GitHub](https://github.com/openai/swarm))

- **CrewAI hierarchical process.** A manager agent reads the goal, breaks it into subtasks, dispatches to workers, synthesizes the final output. Workers run on cheaper models; "the manager has its own LLM (often a stronger reasoning model) and the specialists run on cheaper models." Memory caching reduces cost ~30%. ([CrewAI hierarchical docs](https://docs.crewai.com/en/learn/hierarchical-process))

- **Anthropic's own multi-agent research system.** Opus 4 orchestrator with Sonnet 4 subagents — a 90.2% performance lift over a single Opus call, at roughly 15× the token cost vs. a chat turn. "The economics only work for high-value research: legal due diligence, competitive intelligence, biomedical literature review. Consumer-grade Q&A cannot absorb the multiplier." ([Anthropic multi-agent system writeup](https://blog.bytebytego.com/p/how-anthropic-built-a-multi-agent))

Common pattern across all four: the manager / supervisor / orchestrator is the *bigger* model; the workers are smaller. Comms's current role is structurally the *worker* (it does the relay), not the orchestrator. Putting Opus there is upside-down relative to the prior art.

## 5. Cost + latency math

The Anthropic prompt cache has a 5-minute TTL refreshed on hit; the cached prefix is the system prompt + role + early memory ([cache doc — local memory](../docs/clear-policy.md)). Cache reads are roughly 10× cheaper than fresh prefix tokens.

**Single-model baseline (Sonnet comms today, hypothetically).** A messaging turn pays ~2.5k cached prefix tokens + ~500 fresh tokens (the incoming mail body + the summary + bash call). Within the cache window, cost is dominated by the fresh tokens. Latency: 1.5–3 s for a Sonnet turn at this size.

**Option (a) — Haiku comms + Opus planner peer.**
- Routine messaging turn: cache hit on Haiku's smaller-prefix prompt. Token cost drops roughly 4× vs Sonnet on the same body. Latency drops from 1.5–3 s to 0.5–1 s.
- Planning turn: comms mails planner, waits for reply, executes. Planner's prefix is *its own* cache, never warm at first call. Each `/dispatch` pays:
  - Comms mail → planner: ~0.5 s Haiku
  - Planner first turn: cold-cache Opus, ~1.5k system + plan body. ~3–5 s, ~$0.03–0.05 in tokens.
  - Planner subsequent turns within 5 min: warm cache, ~1–2 s, ~10× cheaper.
  - Recipe → comms: 0.5 s Haiku.
- Total per `/dispatch`: 4–6 s (vs. 1.5–3 s today on Sonnet).
- **Cache trap**: if planner is consulted rarely (less than once per 5 min), every consult is a cold-cache Opus call. The 5-min TTL means infrequent planning *defeats* the cache. Wishful daydream: pin the planner cache via a periodic ping. Per Anthropic's pricing model, that's a real lever — at the cost of a low-rate background ping.

**Option (b) — Sonnet comms.** Identical to today if the role file is the truth. If sulin's "Opus" premise is right, this is a 3–5× cost reduction for zero latency cost. **The cheapest win, conditional on the premise being correct.**

**Option (c) — Haiku comms + ephemeral Opus subagent.** Subagent has *no cache* — each spawn re-pays full prefix. ~$0.05–0.10 per `/dispatch` in tokens, ~3–5 s latency for the subagent's first turn. Roughly equivalent cost to option (a) on a cold call, more expensive when planning fires repeatedly within 5 min. Operationally simpler (no peer agent to keep alive).

**Option (d) — Haiku + cached planner rubric.** Pure Haiku cost. Latency: same as routine messaging. **The cheapest option by far**, but it leans on the rubric carrying the planning quality. Haiku following a thorough decision rubric gets *most* of the way to Opus on `/dispatch`; the gap shows on novel or ambiguous cases.

## 6. Failure modes

What breaks when comms is small?

- **Lost context across the comms / planner boundary.** A planning call from comms to planner sends a snapshot of what comms knows. If comms's snapshot misses a nuance ("kvothe is actually waiting on bast"), the planner's recipe is wrong. CrewAI hits this too — there's a known class of bug "hierarchical process delegation fails — manager agents cannot delegate to worker agents" without explicit memory plumbing.

- **Planner staleness.** If planner has its own cache and last advised 8 minutes ago, the next consult pays a cold-cache miss. With 5-min TTL, sporadic planning is the worst regime for caching.

- **Handoff overhead masking simple cases.** Comms might over-delegate to planner for trivia. Haiku is generally good at "should I escalate this" but can be overcautious.

- **Approval gate drift.** The current role's non-negotiable approval rule depends on comms holding the full thread context. Splitting context across two models risks one of them losing the gate semantics. The rule needs to live in comms's prompt unconditionally.

- **Duplicate work.** If planner and comms both summarize the same incoming mail (comms for the human, planner for its own context), tokens are paid twice.

- **The "what is comms saying right now" debugging surface.** Today one pane = one model = one transcript. Adding a planner peer doubles the inspection surface. Worth a `bus introduce planner` flow.

## 7. Recommendation

**Option (a) Haiku comms + Opus planner peer, contingent on the cache-pin pattern.**

Reasoning:
1. The volume split (80–85% messaging) makes the cheap-relay win large in absolute cost.
2. Prior art is consistent: the smart model belongs in the planner, not the relay. Anthropic's own system uses Opus-as-orchestrator with smaller subagents and gets a 90% performance lift at 15× token cost — exactly the trade-off this proposal makes the *opposite* of (small relay, smart orchestrator), so we get most of the structural benefit at a fraction of the multiplier.
3. The bus already exists. Adding a planner agent is *the same shape* as adding any other agent — no new infrastructure.

**The cache-pin caveat is real.** If planning calls fire less than once every 5 minutes (the cache TTL), option (a) loses to option (b) on cost. Mitigation: a low-rate `planner_heartbeat` that touches the planner's session every ~4 min during active periods. Costs maybe a few hundred tokens per heartbeat; pays back the first time a planning call would have hit a cold cache.

**Phased rollout:**

1. **Confirm the baseline.** Verify whether comms is actually running Opus today (sulin's premise) or Sonnet (per the role file). If Sonnet, the framing of this proposal shifts: option (b) is already the world we live in and the question becomes whether to drop further to Haiku + planner.
2. **Add a planner agent to the bus, still on Opus.** No model change to comms yet. Hook one slash command (`/dispatch`) to consult planner. Verify the round-trip latency and the recipe quality are acceptable.
3. **Swap comms's model to Haiku.** Watch the messaging-quality bar: scan for missed nuances, summary regressions, parse failures on edge cases. Roll back at the first persistent regression.
4. **Add cache-pin if planning frequency is below 1/5min.** Otherwise, leave alone.
5. **Sunset the planner peer if option (d) — Haiku + rubric — proves sufficient.** This is the cleanest end-state if the rubric is good enough; planner-peer is a fallback if Haiku alone falls short on planning.

## Open ambiguities

These I'd want sulin's read on before any model swap:

- **Is comms actually on Opus today?** The role file says Sonnet. Either the file is stale or a launcher overrides. (See §2.)
- **sulin's interrupted "maybe we have an…" thought.** Comms flagged this — sulin had another idea that didn't land in the prompt. Worth asking before committing to a shape that might cut across whatever was being proposed.
- **How is the planner addressed?** Is it `bus msg mail planner "<rubric>"` and we wait? Or is it a synchronous RPC layered above the bus? The async bus path is the natural fit but adds a coordination loop in `/dispatch`.
- **Approval gate semantics for planner calls.** Does sulin need to approve the *consult of the planner*, or just the final outbound? My read is the latter (planner is internal scaffolding) but it's worth being explicit.
