# Tracking, Tracing & Observability — A Deep Reference for claude-bus

> Written 2026-05-28. The deep version of what `modern-agent-techniques.md §3.5`
> sketched in one paragraph. Every technique leads with the **principle** (the
> mechanism, the failure it prevents, the tradeoff), then the concrete shape and
> the exact field names. Skim the tables; read the "How this maps to claude-bus"
> section last — it's one section, not the whole doc.
>
> Companion to `docs/observability-research.md` (elodin, 2026-05-27), which
> scoped the *fleet-tracking* design space (statusline sidecar vs transcript
> scraper). This doc goes one layer down: the *telemetry contract* — OTel GenAI
> semantic conventions, cross-agent distributed tracing, cost/token attribution,
> anomaly detection, and replay/audit. Where that doc asked "where do we read the
> numbers," this one asks "what is the standard shape of those numbers, what does
> the industry build on top, and what does claude-bus get for free that it isn't
> claiming."

---

## 0. The thesis in one breath

claude-bus already has **two of the three observability planes** the industry
converged on, and is missing the third:

| Plane | Industry term | claude-bus today | Gap |
|---|---|---|---|
| **Per-agent live state** | dashboards / gauges | `bus monitor`, `agent-bar`, `$STATE/status/<agent>.json` | none — this is strong |
| **Cross-agent event stream** | structured logs / events | `events.jsonl` (hook-emitted JSONL) | untyped, regex-read, no token data |
| **Causal trace across agents** | distributed tracing (spans) | **absent** | no parent/child causality; can't answer "this prompt → these 3 subagent calls → this cost" |

The single highest-leverage realization: **Claude Code already emits the third
plane for free, in beta, and claude-bus exports none of it.** Flip
`CLAUDE_CODE_ENABLE_TELEMETRY=1` + `CLAUDE_CODE_ENHANCED_TELEMETRY_BETA=1` and
every agent emits a `claude_code.interaction` root span with child
`llm_request` / `tool` / `hook` spans, **subagent spans nested under the parent
tool span**, and W3C `TRACEPARENT` propagation into Bash subprocesses. That is
exactly the cross-agent causal view that `events.jsonl` structurally cannot give
you, and it requires *zero* new instrumentation code — only environment
variables in the file you already symlink fleet-wide.

The second realization: the home-grown `$STATE/status/<agent>.json` token
watcher (`delivery.cpp::maybeScanTokens`) is a **re-derivation of
`claude_code.token.usage` and the `gen_ai.usage.*` semconv**, done by regex over
transcript JSONL, with three latent correctness bugs (§5). It earns its place
as a *zero-dependency live gauge*, but it should be understood as "a local
re-implementation of a standard," not a bespoke thing.

---

## 1. Core principles & tradeoffs

### 1.1 The three signals are not interchangeable — they answer different questions

**The principle.** OpenTelemetry's separation into **metrics / logs (events) /
traces** is not bureaucratic; each answers a question the others structurally
can't, and conflating them is the root cause of most ad-hoc observability pain.

- **Metrics** answer *"how much / how fast, aggregated"* — token spend per hour,
  p95 latency, error rate. Cheap to store (pre-aggregated time series), cheap to
  query, but **dimensionless about causality**: a metric tells you the fleet
  burned 2 M tokens this hour, never *which prompt* caused it.
- **Events / logs** answer *"what happened, discretely"* — this prompt was
  submitted, this tool was accepted, this API call cost $0.04. Your
  `events.jsonl` is exactly this plane. High cardinality, replayable, but
  **flat**: no record knows it was *caused by* another record.
- **Traces (spans)** answer *"what caused what, across process boundaries"* —
  this user prompt (root span) spawned this LLM call (child) which invoked this
  tool (child) which spawned this subagent (grandchild). The parent/child edges
  *are* the value; they're what let you say "the 50K-token blowup happened
  inside the `code-review` subagent's third tool call."

The tradeoff: traces are the most expensive (one span per operation, full
context propagation) and the youngest standard (GenAI semconv still
"Development" / Claude Code traces still "beta"). The discipline is **emit all
three but read the cheapest one that answers your question** — gauge from
metrics, audit from events, debug-causality from traces.

**Why this matters for claude-bus specifically.** Your CLAUDE.md already names
"two complementary channels: live scrollback + event log." That's the per-agent
gauge plane and the event plane. The *missing* plane — traces — is precisely the
"cross-agent view that scrollback can't give you" the doc gestures at. You
described the gap correctly and then built only the event half of it.

### 1.2 Don't own the call site — you can only observe the boundaries

**The principle.** Every framework observability stack (LangGraph, CrewAI,
AutoGen, openllmetry) instruments *the LLM call site* — it wraps
`client.messages.create()` and emits a span around it. claude-bus **does not own
that call site**; Claude Code does. This is the structural fact elodin's doc
identified, and it has a sharp consequence: you have exactly two integration
seams, and they are both *outputs* of Claude Code, never *wrappers* around it.

1. **The telemetry seam** — Claude Code's own OTel exporter (metrics/events/
   traces over OTLP). This is the *blessed* call-site instrumentation, written
   by the people who own the call site. You consume it; you don't reproduce it.
2. **The artifact seam** — files Claude Code writes as a side effect: the
   transcript JSONL (`message.usage`), the statusline JSON payload, the hook
   payloads. This is what every community tool (ccusage, claude-usage,
   Usage-Monitor) reads, and what your token watcher reads.

The tradeoff between the two seams is **freshness + standardization (telemetry)
vs zero-dependency + retroactive (artifact)**. The telemetry seam needs a
collector running; the artifact seam works on a bare machine and can re-parse
last week's session. The right architecture uses *both for what each is good at*
(see §5).

### 1.3 Token accounting has four buckets, and collapsing them lies to you

**The principle.** A "token count" is meaningless without its bucket. The
Anthropic usage block has four, and they have **10× cost differences**:

| Bucket (transcript field) | semconv / CC name | Cost weight | What it means |
|---|---|---|---|
| `input_tokens` | `gen_ai.usage.input_tokens`, CC `type=input` | 1× | fresh prompt tokens |
| `output_tokens` | `gen_ai.usage.output_tokens`, CC `type=output` | ~5× input | generated tokens |
| `cache_creation_input_tokens` | CC `type=cacheCreation` | 1.25× input | tokens written to the 5-min cache |
| `cache_read_input_tokens` | CC `type=cacheRead` | **0.1× input** | tokens re-read from cache |

**Context occupancy** (how full is the window) = `input + cache_creation +
cache_read` (everything that isn't output). **Cost** = a *weighted* sum with the
per-bucket multipliers. These are different questions: an agent can be at 95%
context (occupancy) while costing almost nothing (because 90% of it is
cache-read). Your token watcher computes occupancy correctly
(`delivery.cpp:973-976` sums exactly those three) but **cannot compute cost**
because cost needs the weights and the per-model price table — which is why every
serious tool (ccusage) ships a pricing table and Claude Code's
`claude_code.cost.usage` counter exists. The principle: **occupancy is free from
the transcript; cost is not — it requires pricing knowledge.** Don't fake cost by
summing tokens.

### 1.4 Cardinality is the silent killer of metrics

**The principle.** A metric's storage and query cost scales with the
**cardinality** of its attribute combinations (number of distinct label tuples).
Putting a unique ID (`session.id`, `msg_id`, `request_id`) on a *metric* explodes
cardinality — every session becomes a new time series. This is why Claude Code
ships explicit **cardinality-control env vars**: `OTEL_METRICS_INCLUDE_SESSION_ID`
(default true), `OTEL_METRICS_INCLUDE_VERSION` (default false),
`OTEL_METRICS_INCLUDE_ACCOUNT_UUID`, `OTEL_METRICS_INCLUDE_ENTRYPOINT`. High-
cardinality identifiers belong on **events and span attributes** (where each
record is independent), never on metric labels. The discipline: *aggregate*
dimensions (model, agent role, tool name) go on metrics; *identity* dimensions
(session, request) go on events/spans.

---

## 2. How the best implementations actually do it

### 2.1 OpenTelemetry GenAI semantic conventions — the vocabulary everyone agrees on

**The principle.** Ad-hoc dashboards don't compose across tools; a *standard
vocabulary* does. The OTel GenAI semconv (status: Development, stabilizing
through 2026) defines the span/metric/attribute names that Langfuse, Braintrust,
Datadog, Honeycomb, and Claude Code itself all speak, so a trace emitted by one
is readable by all.

**Span names** (verbatim from the spec):

| Operation | Span name template | Span kind |
|---|---|---|
| inference | `{gen_ai.operation.name} {gen_ai.request.model}` → e.g. `chat claude-opus-4-8` | CLIENT |
| embeddings | `{gen_ai.operation.name} {gen_ai.request.model}` | CLIENT |
| tool execution | `execute_tool {gen_ai.tool.name}` | INTERNAL |
| agent invocation | `invoke_agent {gen_ai.agent.name}` (or bare `invoke_agent`) | INTERNAL |
| agent creation | `create_agent {gen_ai.agent.name}` | INTERNAL |

`gen_ai.operation.name` allowed values: `chat`, `generate_content`,
`embeddings`, `execute_tool`, `invoke_agent`, `create_agent`, `retrieval`,
`text_completion`, `invoke_workflow`.

**Attributes you care about** (exact names):

- Identity/causality: `gen_ai.conversation.id` (correlates messages),
  `gen_ai.agent.id`, `gen_ai.agent.name`, `gen_ai.agent.description`.
- Request: `gen_ai.provider.name` (= `anthropic`), `gen_ai.request.model`,
  `gen_ai.response.model`, `gen_ai.response.id`, `gen_ai.response.finish_reasons`.
- Usage: `gen_ai.usage.input_tokens`, `gen_ai.usage.output_tokens`.
- Tool: `gen_ai.tool.name`, `gen_ai.tool.call.id`, `gen_ai.tool.type`.
- Error: `error.type` (conditionally required when an error occurred).
- Transport: `server.address`, `server.port`.

**Metrics** (exact names, instrument types, units):

| Metric | Type | Unit | Required attrs |
|---|---|---|---|
| `gen_ai.client.token.usage` | Histogram | `{token}` | `gen_ai.operation.name`, `gen_ai.provider.name`, `gen_ai.token.type` (=input/output) |
| `gen_ai.client.operation.duration` | Histogram | `s` | `gen_ai.operation.name`, `gen_ai.provider.name` |
| `gen_ai.client.operation.time_to_first_chunk` | Histogram | `s` | streaming only |
| `gen_ai.server.time_per_output_token` | Histogram | `s` | server-side |

The token histogram's bucket boundaries are token-scale powers (1, 4, 16, 64,
256, 1024, … 67M); the duration histogram's are sub-second-to-80s. **Note the
design choice**: token usage is a *histogram*, not a counter — so you can ask
"p95 tokens per request," not just "total tokens." That distribution view is how
you catch the "50K-token answer to a 3K question" anomaly (§4).

Sources: [GenAI spans](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-spans/),
[GenAI agent spans](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-agent-spans/),
[GenAI metrics](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-metrics/).

### 2.2 Claude Code's built-in OTel — the exact surface

**The principle.** The vendor who owns the call site emits the call-site
instrumentation. Claude Code exports **all three signals**, gated by env vars,
and the schema is stable enough to build on.

**Metrics** (instrument: counter unless noted; every metric carries the standard
attributes):

| Metric | Unit | Key extra attributes |
|---|---|---|
| `claude_code.session.count` | count | — |
| `claude_code.token.usage` | tokens | `type` ∈ {input, output, cacheRead, cacheCreation}, `model`, `query_source` ∈ {main, subagent, auxiliary}, `agent.name`, `skill.name`, `mcp_server.name`, `effort`, `speed` |
| `claude_code.cost.usage` | USD | `model`, `query_source`, `effort`, `agent.name`, … |
| `claude_code.lines_of_code.count` | count | — |
| `claude_code.pull_request.count` / `.commit.count` | count | — |
| `claude_code.code_edit_tool.decision` | count | `tool_name`, `decision` ∈ {accept, reject}, `source`, `language` |
| `claude_code.active_time.total` | s | `type` ∈ {user, cli} |

**Crucial attribution detail:** `query_source` and `agent.name` mean **the
token/cost counters are already broken out by subagent vs main thread.** You can
ask "how many tokens did the `code-review` subagent burn" from the metric alone.
This is *exactly* the per-agent cost attribution elodin's doc wanted, and it's
native.

**Events** (over the logs/OTLP channel; each has `event.name`, ISO `event.timestamp`,
monotonic `event.sequence` for in-session ordering):

- `claude_code.user_prompt` — `prompt` (redacted unless `OTEL_LOG_USER_PROMPTS=1`),
  `user_prompt_length`.
- `claude_code.tool_result` — `tool_name`, `decision_type` (always `accept`,
  since rejected tools produce no result), `tool_result_size_bytes`, success flag.
- `claude_code.api_request` — `model`, `cost_usd`, `duration_ms`, `input_tokens`,
  `output_tokens`, `cache_read_tokens`, `cache_creation_tokens`, `request_id`
  (the Anthropic `req_011…` id), `query_source`, `effort`, `speed`,
  `agent.name`/`skill.name`/`mcp_server.name`.
- `claude_code.api_error` — model, error category, duration, status.
- `claude_code.tool_decision` — `decision` ∈ {accept, reject}, `source`.

**This is a strict superset of `events.jsonl` plus the token data hooks lack.**
Your hooks emit no `input_tokens`; `claude_code.api_request` emits all four
buckets *and* `cost_usd` *and* the upstream `request_id`.

**Traces (beta)** — enabled by `CLAUDE_CODE_ENABLE_TELEMETRY=1` +
`CLAUDE_CODE_ENHANCED_TELEMETRY_BETA=1` + `OTEL_TRACES_EXPORTER=otlp`. The span
hierarchy (verbatim):

```
claude_code.interaction               # one per user prompt (root)
├── claude_code.llm_request           # model + token + cache attrs, ERROR on failure
├── claude_code.hook                  # requires detailed beta tracing
└── claude_code.tool
    ├── claude_code.tool.blocked_on_user   # time waiting on permission decision
    ├── claude_code.tool.execution         # the actual run; error category attr
    └── (Task tool) subagent claude_code.llm_request / claude_code.tool spans
```

`claude_code.llm_request` attributes: `model`, `gen_ai.system` (=`anthropic`),
`gen_ai.request.model`, `llm_request.context` ∈ {interaction, tool, standalone},
`input_tokens`, `output_tokens`, `cache_read_tokens`, `cache_creation_tokens`,
`gen_ai.response.id`, `gen_ai.response.finish_reasons`. Each retry is a
`gen_ai.request.attempt` span event. **So Claude Code already dual-emits the
GenAI semconv names alongside its own** — a trace is portable to any OTel backend.

**The two cross-process propagation mechanisms — this is the cross-agent story:**

1. **Subprocess propagation:** when tracing is active, Bash/PowerShell
   subprocesses inherit a `TRACEPARENT` env var with the active tool span's W3C
   context. Any subprocess that reads it parents *its own* spans under the same
   trace.
2. **SDK/`-p` inbound propagation:** a `claude -p` or Agent SDK session *reads*
   `TRACEPARENT`/`TRACESTATE` from its env and makes its `claude_code.interaction`
   a child of the caller's span. (Interactive sessions deliberately *ignore*
   inbound `TRACEPARENT` to avoid inheriting ambient CI values.)

The consequence for a multi-agent harness: **if agent A spawns agent B via a
process where A can set `TRACEPARENT`, B's entire trace nests under A's.** That
is true cross-agent distributed tracing, built in. The catch for claude-bus is
that your agents are *interactive* TUIs (which ignore inbound TRACEPARENT) and
they're peers, not parent/child processes — so you'd correlate them by a shared
*conversation/session resource attribute* rather than span parentage (§5.4).

Source: [Claude Code monitoring](https://code.claude.com/docs/en/monitoring-usage).

### 2.3 ccusage — the reference transcript parser, and its dedup invariant

**The principle.** When you read an append-only artifact that the producer may
write *more than once* (retries, resumed sessions re-logging, the same line
flushed twice), you must **dedup by a stable composite key** or you double-count.
ccusage — the canonical Claude Code usage tool — reads the per-session JSONL
under `~/.claude/projects/<encoded-cwd>/<uuid>.jsonl` and, for each assistant
line, reads `message.usage.{input_tokens, output_tokens,
cache_creation_input_tokens, cache_read_input_tokens}`, `message.id`,
`message.model`, top-level `requestId`, `costUSD`, `timestamp`, `sessionId`.

The dedup invariant (from the test fixtures in
`rust/crates/ccusage/src/main.rs`): a usage record is uniquely keyed by
**`message.id` + `requestId`** (composite hash). Two lines with the same
`msg_123`/`req_456` pair are the *same* API result logged twice and counted once.
Two lines with the same `message.id` but *different* `requestId` are distinct
attempts. It also **filters the synthetic `<synthetic>` model** (Claude Code
writes placeholder usage rows that aren't real API calls). Cost comes from a
per-model pricing table with separate `cache_read` / `cache_creation` /
`cache_read_above_200k` tiers — confirming §1.3's "cost needs weights."

**This is the spec your token watcher silently violates** (§5.1): it keeps only
the *last* assistant line's occupancy with no dedup, which is fine for "current
occupancy" but wrong the moment you sum for cumulative.

Source: [ccusage](https://github.com/ryoppippi/ccusage)
(`rust/crates/ccusage/src/{utils,cost,main}.rs`).

### 2.4 Langfuse / Braintrust / openllmetry — the trace-storage layer

**The principle.** Once you emit GenAI spans, you need a store that models the
*hierarchy* and *grouping*, not just flat rows. The convergent model:

- **Langfuse:** three nested concepts — **observations** (span / generation /
  event) live inside a **trace** (one request), and a **session** groups *many
  traces* via a propagated `sessionId` (1:n). A `generation` observation carries
  model + token usage + computed cost (Langfuse multiplies token counts by a
  per-model price table — same as ccusage). Replay/eval: you re-run prompts over
  historical traces and diff outputs. The session abstraction is the key one for
  multi-agent: **one long-lived agent = one session; each turn = one trace.**
  ([Langfuse sessions](https://langfuse.com/docs/observability/features/sessions))
- **openllmetry (traceloop):** an OTel SDK that auto-instruments the LLM call and
  emits exactly the GenAI semconv spans — the "emit" half that Langfuse "stores."
  Its value is showing the semconv is *real and adopted*, not aspirational.
- **Braintrust:** adds the *eval* layer on top of traces — scoring functions over
  logged spans, so "did this agent get worse" becomes a regression test, not a
  vibe. ([Braintrust 2026 tooling review](https://www.braintrust.dev/articles/best-llm-monitoring-tools-2026))

The transferable idea for claude-bus: **the OTel `gen_ai.conversation.id` /
Langfuse `sessionId` is the join key that turns N independent agent streams into
one queryable fleet.** You already have a stable per-agent session UUID
(agent-launch resolves it); that UUID *is* your session id.

---

## 3. The design space

### 3.1 Where to put the telemetry — five substrates

| Substrate | Freshness | Retroactive? | Standardized? | Dep cost | Best for |
|---|---|---|---|---|---|
| **Claude Code OTel → local collector** | live (5–60s) | no (only while running) | yes (semconv) | a collector process | cross-agent traces, cost/anomaly |
| **Transcript JSONL scrape** (ccusage-style) | ~1 turn lag | **yes** | no | none | retroactive audit, occupancy gauge |
| **Statusline JSON sidecar** | per-turn | no | no (CC-private schema) | settings change | live cost/ctx/rate-limit gauge |
| **Hook → `events.jsonl`** | per-event | yes (the log persists) | no | none | cross-agent *event* timeline (no tokens) |
| **Broker-derived state RPC** | 250ms–5s | no | no | already built | live lifecycle dashboard |

The honest reading: claude-bus uses the **bottom three** (sidecar-derived status,
hooks, broker RPC) and ignores the **top two** (OTel, transcript-scrape-for-cost).
The bottom three are great for the *live gauge plane*; they're structurally
incapable of the *causal trace plane* and the *standardized cost plane*.

### 3.2 The latency/cost/standardization triangle

Every observability decision trades among three:

- **Latency** — how fast does the signal reach the viewer? (broker RPC: 250ms;
  status file: 5s; OTel metrics: up to 60s.)
- **Cardinality/cost** — how much does it cost to store/query at fleet scale?
- **Standardization** — can another tool read it without bespoke parsing?

claude-bus optimizes hard for **latency** (the 250ms delivery loop, the 5s token
scan, the 1Hz monitor) because the use case is a human watching live. It
sacrifices **standardization** (everything is bespoke JSON read by bespoke regex)
and never paid the **cost** axis because the fleet is tiny. The OTel adoption
move is precisely: *keep the low-latency bespoke path for the live human gauge,
add the standardized path for everything that wants to be queried/replayed/
alerted later.* They are not in competition — they're the gauge plane and the
analytics plane.

### 3.3 Push vs pull for token data

- **Pull (today):** the broker *polls* every 5s, tailing each live transcript.
  Simple, no Claude Code cooperation, but lags and burns IO scanning unchanged
  files (mitigated by the offset cache in `maybeScanTokens`).
- **Push via statusline:** Claude Code *pushes* the full computed JSON (incl.
  `used_percentage`, `cost.total_cost_usd`, `rate_limits`) to a script on every
  turn — exactly the data the watcher re-derives, plus cost and rate-limits the
  transcript can't give. elodin's Design A.
- **Push via OTel:** Claude Code *pushes* metrics/events/spans to a collector.

The principle: **the transcript scrape re-derives a number Claude Code already
computed and is willing to hand you.** The statusline `used_percentage` is
authoritative (Claude Code knows the real window size; your watcher *guesses* it
via `CLAUDE_BUS_CTX_WINDOW` + a tier-escalation hack, §5.2). Pull made sense as a
zero-config bootstrap; push is strictly more accurate once you accept one
settings change.

---

## 4. Novel ideas worth considering

### 4.1 Token-anomaly detection as a broker capability

**The principle.** "One runaway agent loop can burn a monthly budget in hours"
— the 2026 consensus is that **real-time anomaly detection on token-rate is
survival, not luxury.** The detectable signatures
([Galileo](https://galileo.ai/blog/ai-agent-cost-optimization-observability),
[oneuptime](https://oneuptime.com/blog/post/2026-03-14-monitoring-ai-agents-in-production/view)):

| Anomaly | Signal | Detection |
|---|---|---|
| **Cost-rate spike** | "$50/hr, normally $3/hr" | rolling baseline (same hour, last 7 days); fire when ratio > 2× |
| **Loop / runaway** | "20+ LLM calls for one query" | count `api_request` events between two `user_prompt` events |
| **Prompt bloat** | rising avg input_tokens per turn | trend on `input_tokens` histogram |
| **Tool-failure storm** | tool failure rate > 20% rolling | ratio of failed `tool_result` to total |

claude-bus is *uniquely positioned* to do this: the broker already scans
`events.jsonl` every loop and already has an escalation channel (`audit` topic +
`inbox-human`). A `Loop::maybeDetectAnomaly()` sibling to `maybeScanTokens()`
could track per-agent token deltas and llm-call counts between prompts, and on a
runaway signature do exactly what the self-healing supervisor (modern-techniques
§3.4) does: nudge, `/compact`, or escalate. **This is the killer app of the token
data you're already collecting** — you scan it for a gauge but never *alert* on it.

### 4.2 Replayable audit from the event log (event sourcing for observability)

**The principle.** An append-only event log is a *replayable* substrate: feed the
same events through a pure reducer and you reconstruct any past state. This is the
same insight as modern-techniques §1.2 (typed state reducer), applied to
*observability* rather than *delivery*. If `events.jsonl` carried token deltas
(it doesn't today — hooks lack them), you could replay "what was the fleet's cost
at 14:32 yesterday" by folding events up to that timestamp. Two ways to get the
token data into the replayable log:

1. A `Stop`/`PostToolUse` hook tails the transcript and appends a synthetic
   `Tokens` event (elodin's Design C) — cheap, keeps the log self-contained.
2. Subscribe a local OTLP collector → file exporter, and treat *that* as the
   replayable log (standardized, but a second log).

The principle worth stealing from Langfuse: **a `session` (your agent UUID) is
the replay unit.** "Replay everything elodin did in session X" should be one
query over a log keyed by session id.

### 4.3 A `bus trace` viewer — the missing third plane

**The principle.** You have `bus monitor` (gauge), `bus events` (event timeline),
and no *causal* viewer. With Claude Code traces on, a `bus trace [AGENT]` could
render the `claude_code.interaction` → llm_request → tool → subagent tree for the
agent's last turn, *live in a pane* — the human watching causality unfold. This
is the differentiator the doc keeps naming: a headless Langfuse UI shows you the
tree *after*; a pane shows it *as it happens*, and you can grab the keyboard.

### 4.4 Cost attribution per *bus task*, not just per agent

**The principle.** Claude Code attributes cost per session/subagent. But a
claude-bus "task" spans *multiple agents* (dispatch fan-out → workers → judge).
If the `dispatch` skill stamped a shared correlation id (a `OTEL_RESOURCE_ATTRIBUTES`
custom attribute like `bus.task_id=…`) into each spawned agent's environment, the
collector could roll up "this dispatched task cost $1.40 across 4 agents." This is
the multi-agent generalization of `gen_ai.conversation.id`, and
`OTEL_RESOURCE_ATTRIBUTES` is the exact documented hook for it
(`export OTEL_RESOURCE_ATTRIBUTES="bus.task_id=t-123,team.id=fleet"`).

### 4.5 Distinguish occupancy from cost in the monitor

**The principle (from §1.3).** The `CTX` column shows occupancy %. It does *not*
show cost-rate. An agent at 30% occupancy doing huge fresh-input turns is
*expensive*; an agent at 95% occupancy that's all cache-read is *cheap*. A second
column (cost-rate $/turn, from `claude_code.cost.usage` or statusline
`cost.total_cost_usd` deltas) would surface the expensive-but-not-full agent the
CTX column hides.

---

## 5. How this maps to claude-bus

This is the action section. Each item leads with the principle, then the concrete
change and effort/payoff. Flaws in current code are flagged with file refs.

### 5.1 FLAW — the token watcher has no dedup and can't be summed (`delivery.cpp:959-980`)

**Principle:** appending-artifact readers must dedup by composite key (§2.3).
`maybeScanTokens` keeps only the *last* assistant line's occupancy
(`sc.last_tokens = …` overwrites each loop), which is correct for "current
occupancy" but means the field is **un-summable** — the moment anyone wants
cumulative tokens or cost, this code double-counts retries and resumed-session
re-logs. It also doesn't filter the `<synthetic>` model rows ccusage filters.
*Today this is latent* (only occupancy is consumed), but it's a trap for anyone
extending it to cost. **Fix:** if you add cumulative/cost, adopt ccusage's
`message.id`+`requestId` dedup set and the `<synthetic>` filter. Effort: low.
Payoff: med (unblocks cost).

### 5.2 FLAW — the window denominator is guessed, not authoritative (`delivery.cpp:925-990`)

**Principle:** don't re-derive a number the producer computes and will hand you
(§3.3). The watcher computes `used_percentage` as `tokens / window` where
`window` comes from `CLAUDE_BUS_CTX_WINDOW` (default 200k) plus a
**tier-escalation hack** (`sc.last_tokens > 200'000 ? 1'000'000 : 200'000`,
line 988). This is fragile: a 1M-context model below 200k tokens reports against
the wrong denominator, and the hack hard-codes two tiers. Claude Code's
statusline payload carries the *real* `context_window.used_percentage` and
`context_window_size` (elodin's research §1.1). **Fix:** prefer the statusline
sidecar (elodin's Design A) for the percentage; keep the transcript scrape only
as the fallback when no statusline is installed. Effort: low (the status JSON
shape already matches what `contextStatsFor` reads in `sub_monitor.cpp:244`).
Payoff: med (correct % on every tier, plus free cost + rate-limits).

### 5.3 FLAW — `events.jsonl` is untyped and read by hand-rolled regex (`agent_status.cpp:29-55`, `sub_events.cpp:43-69`, `sub_monitor.cpp:136-148`)

**Principle:** parse structured data with a parser, at the boundary, once
(modern-techniques §1.2/1.4). There are **three separate hand-rolled
`extractField`/`extractStr` substring scanners** across the codebase, each with
its own escape-handling quirks (agent_status handles `\n\r\t`, sub_monitor's
"naive" one doesn't), each doing flat substring search that can match a key
inside a nested payload by accident (the comment at `agent_status.cpp:393` even
admits "extractField does a flat substring search so nesting is fine" — which is
only true by luck of field names). **Fix:** one `Event` struct parsed by
`json_min` at read time; viewers consume typed fields. This is the prerequisite
for putting token data *into* the event stream (§4.2). Effort: med. Payoff: high
(kills a class of silent misparse, dedups three copies).

### 5.4 ADOPT — turn on Claude Code OTel fleet-wide (the big one)

**Principle:** consume the blessed call-site instrumentation; don't reproduce it
(§1.2). The change is **env vars in `settings/claude-settings.json`** (the file
you already symlink fleet-wide) plus a local collector. Metrics + events get you
per-agent/per-subagent token & cost attribution *for free* via `query_source` /
`agent.name`; the beta traces get you the causal plane.

Concretely add to settings (or the fleet layout's env):
```
CLAUDE_CODE_ENABLE_TELEMETRY=1
OTEL_METRICS_EXPORTER=otlp
OTEL_LOGS_EXPORTER=otlp
OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317
OTEL_RESOURCE_ATTRIBUTES=service.name=claude-bus   # + bus.task_id per dispatch
# opt into the causal plane:
CLAUDE_CODE_ENHANCED_TELEMETRY_BETA=1
OTEL_TRACES_EXPORTER=otlp
```
Caveat to flag: Claude Code **does not pass `OTEL_*` to subprocesses** (hooks,
Bash) — so this instruments *the agent*, not your hooks. And **interactive
sessions ignore inbound `TRACEPARENT`**, so peer agents won't auto-nest; correlate
them by a shared resource attribute (`bus.task_id`) instead of span parentage.
Effort: low (env vars) + med (run a collector, e.g. an otel-collector container or
the prometheus exporter). Payoff: **very high** — standardized cost/token/anomaly
data and the missing trace plane, with no new C++.

### 5.5 ADOPT — token-anomaly detection in the broker loop

**Principle:** you already collect the data and have an escalation channel; alert
on it (§4.1). Add a `Loop::maybeDetectAnomaly()` that, per agent, tracks token
deltas and `api_request`/llm-call counts between `UserPromptSubmit` events, and on
a runaway signature (calls > N per prompt, or token-rate > 2× rolling baseline)
appends to `audit` and mails `inbox-human` — reusing the exact escalation path
`maybeAutoClear` already uses (`delivery.cpp:886-903`). Effort: med. Payoff: high
(turns passive token-scanning into active budget protection; pairs with the
self-healing supervisor).

### 5.6 ADOPT (cheap) — separate occupancy from cost-rate in `bus monitor`

**Principle:** §1.3 — occupancy and cost are different questions. Add a cost-rate
column sourced from the statusline `cost.total_cost_usd` delta (once §5.2's
sidecar lands) or `claude_code.cost.usage` (once §5.4 lands). The CTX color tiers
in `sub_monitor.cpp:346-350` are good; mirror them for a `$/turn` column. Effort:
low. Payoff: med.

### 5.7 CONSIDER — `bus trace` viewer (the third plane)

**Principle:** §4.3 — give the human the causal view *live in a pane*, which a
headless backend can't. Depends on §5.4 traces. A pane that renders the live
`claude_code.interaction` span tree for an agent is the unique-to-claude-bus
observability surface. Effort: high (needs a collector tap + a tree renderer).
Payoff: med–high (the differentiator, but only after the cheaper wins).

### 5.8 NON-GOALS / cautions

- **Don't rip out the token watcher** — it's the zero-dependency live gauge that
  works on a bare machine with no collector. Demote it to "fallback + occupancy,"
  don't delete it.
- **Don't put `session.id`/`msg_id` on metric labels** (§1.4) — cardinality
  explosion. Keep identity on events/spans; Claude Code's
  `OTEL_METRICS_INCLUDE_SESSION_ID=false` is the knob if you forward metrics.
- **Don't enable `OTEL_LOG_USER_PROMPTS` / `OTEL_LOG_RAW_API_BODIES` on a public
  repo's tracked settings** — those export prompt and full-conversation content.
  CLAUDE.local.md's no-secrets rule extends here: telemetry content gates are
  privacy decisions, default-off for a reason.
- **Don't treat traces as durable storage** — they're beta and sampled; the
  append-log `events.jsonl` remains the crash-safe audit substrate.

### 5.9 Recommended order (payoff ÷ effort)

| # | Move | Effort | Payoff |
|---|---|---|---|
| 1 | Turn on Claude Code OTel metrics+events fleet-wide (§5.4) | low | **very high** |
| 2 | Statusline sidecar → authoritative ctx% + cost + rate-limits (§5.2, elodin Design A) | low | high |
| 3 | Token-anomaly detection in broker loop (§5.5) | med | high |
| 4 | Typed `Event` parse, kill the 3 regex scanners (§5.3) | med | high |
| 5 | Occupancy-vs-cost column in monitor (§5.6) | low | med |
| 6 | Enable beta traces + `bus trace` viewer (§5.4 traces, §5.7) | high | med–high |
| 7 | Dedup + cumulative in token watcher if cost is wanted (§5.1) | low | med |

---

## Sources

**OpenTelemetry GenAI semantic conventions**
- [GenAI spans](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-spans/) — operation names, span naming, attributes
- [GenAI agent spans](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-agent-spans/) — invoke_agent / create_agent, agent.id/name/description
- [GenAI metrics](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-metrics/) — token.usage histogram, operation.duration, buckets
- [GenAI overview](https://opentelemetry.io/docs/specs/semconv/gen-ai/) — signal split (metrics/events/spans)

**Claude Code built-in telemetry**
- [Monitoring usage](https://code.claude.com/docs/en/monitoring-usage) — env vars, all metric/event names, span hierarchy, TRACEPARENT propagation, cardinality controls

**Reference implementations**
- [ccusage](https://github.com/ryoppippi/ccusage) — transcript JSONL parse, msg_id+requestId dedup, per-model+cache pricing (`rust/crates/ccusage/src/{utils,cost,main}.rs`)
- [Langfuse sessions](https://langfuse.com/docs/observability/features/sessions) — trace/observation/session model, session = group of traces
- [Braintrust — best LLM monitoring tools 2026](https://www.braintrust.dev/articles/best-llm-monitoring-tools-2026) — eval-over-traces layer
- [Langfuse OTel integration](https://langfuse.com/integrations/native/opentelemetry) — semconv ingestion

**Anomaly detection / FinOps**
- [Galileo — AI agent cost optimization with observability](https://galileo.ai/blog/ai-agent-cost-optimization-observability)
- [oneuptime — monitoring AI agents in production](https://oneuptime.com/blog/post/2026-03-14-monitoring-ai-agents-in-production/view)
- [oneuptime — observability for AI agents](https://oneuptime.com/blog/post/2026-02-19-observability-for-ai-agents-why-your-llm-apps-are-flying-blind/view)

**Prior internal docs (build on, don't duplicate)**
- `docs/observability-research.md` (elodin, 2026-05-27) — fleet-tracking design space, statusline-sidecar recommendation
- `docs/modern-agent-techniques.md` §3.5 — the one-paragraph OTel sketch this deepens
- `docs/status-decouple.md` — why the token scan moved off the statusline into the broker
