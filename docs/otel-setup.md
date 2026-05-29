# OpenTelemetry for claude-bus — setup & verify

Author: kvothe · 2026-05-28
Status: landed — env wired in `settings/claude-settings.json`; collector is opt-in.

Turns on Claude Code's **built-in** OpenTelemetry so the fleet emits
standardized metrics + events with **per-subagent cost and token
attribution** — for free, with zero new C++ in the bus. Background and the
full telemetry contract live in `.workspaces/comms/docs/deep/observability.md`
(§2.2, §5.4). This doc is the operational how-to.

## What's wired, and why it's safe-by-default

`settings/claude-settings.json` carries an `env` block (symlinked fleet-wide,
so it applies to every agent):

```jsonc
"env": {
  "CLAUDE_CODE_ENABLE_TELEMETRY": "1",
  "OTEL_METRICS_EXPORTER":  "otlp",
  "OTEL_LOGS_EXPORTER":     "otlp",
  "OTEL_EXPORTER_OTLP_PROTOCOL": "grpc",
  "OTEL_EXPORTER_OTLP_ENDPOINT": "http://localhost:4317",
  "OTEL_METRIC_EXPORT_INTERVAL": "10000",   // 10s (vendor default 60000)
  "OTEL_LOGS_EXPORT_INTERVAL":   "5000",    // 5s  (= vendor default)
  "OTEL_RESOURCE_ATTRIBUTES": "service.name=claude-bus"
}
```

**Telemetry is enabled fleet-wide, but the collector is opt-in.** When no
collector listens on `:4317`, Claude Code's OTLP exporter retries in the
background and silently drops — it never blocks the agent, errors a turn, or
writes to the pane. So leaving this on with no collector running costs nothing
and shows nothing. Start the collector when you want the data.

### Content gating — all OFF, deliberately

None of the content gates are set, so all default OFF (verified against
`code.claude.com/docs/en/monitoring-usage`):

- `OTEL_LOG_USER_PROMPTS` — prompt text (default: disabled)
- `OTEL_LOG_TOOL_DETAILS` — Bash commands, tool input, MCP/skill names (disabled)
- `OTEL_LOG_TOOL_CONTENT` — tool input+output bodies; needs beta tracing (disabled)
- `OTEL_LOG_RAW_API_BODIES` — full conversation JSON (disabled)

This repo is **public**. Telemetry content gates are privacy decisions
(`CLAUDE.local.md`'s no-secrets rule extends here) — keep them off in tracked
settings. What we export is counts, costs, durations, model names, and the
`agent.name`/`query_source` attribution — no prompt or tool content.

## Run the collector

```bash
mkdir -p /tmp/claude-bus/otel
nix run nixpkgs#opentelemetry-collector-contrib -- \
  --config /home/sulin/claude-bus/settings/otel/collector.yaml
```

To keep it tied to the session like the broker, launch it as a floating zellij
pane in the ops tab (closing the pane stops it):

```bash
zellij action new-pane --floating -- \
  nix run nixpkgs#opentelemetry-collector-contrib -- \
    --config /home/sulin/claude-bus/settings/otel/collector.yaml
```

The collector writes two append-only JSONL artifacts:

- `/tmp/claude-bus/otel/metrics.jsonl` — `claude_code.token.usage`,
  `claude_code.cost.usage`, `claude_code.session.count`, …
- `/tmp/claude-bus/otel/events.jsonl` — `claude_code.api_request`,
  `claude_code.tool_result`, `claude_code.user_prompt`, …

## Verify

After the collector is up and an agent does one turn:

```bash
# 1. Data is arriving at all:
ls -l /tmp/claude-bus/otel/

# 2. Per-subagent / per-source token attribution is present:
grep -o '"query_source":"[a-z]*"' /tmp/claude-bus/otel/metrics.jsonl | sort | uniq -c
#   → counts for main / subagent / auxiliary

# 3. Cost is being attributed (claude_code.cost.usage, in USD):
grep -c 'claude_code.cost.usage' /tmp/claude-bus/otel/metrics.jsonl

# 4. api_request events carry the four token buckets + cost_usd + request_id:
grep -m1 'claude_code.api_request' /tmp/claude-bus/otel/events.jsonl
```

If `metrics.jsonl` stays empty: confirm the agent was launched *after* the env
block landed (env is read at process start — a running agent won't pick it up),
and that the collector is actually bound to `:4317` (`ss -ltn | grep 4317`).

## The join key, for later

Both the OTel `api_request`/`tool_result` events and the bus's own hook
payloads (`events.jsonl`) carry the same `tool_use_id`. That makes the two
data sources joinable row-for-row: the bus's identity fields (agent-id,
pane-id, session) on one side, Claude Code's tokens/cost on the other. A future
`bus`-side enrichment can fold them into one typed event without a synthetic
token hook. (See deep doc §2.2.)

## Not done here (deliberately out of scope)

- **Beta traces** (`CLAUDE_CODE_ENHANCED_TELEMETRY_BETA=1` + `OTEL_TRACES_EXPORTER`)
  — the causal span plane and a future `bus trace` viewer. Bigger lift; left for
  a follow-up.
- **Per-dispatch cost rollup** — stamping `bus.task_id` into each spawned agent's
  `OTEL_RESOURCE_ATTRIBUTES` so a dispatched task's cost rolls up across agents.
  That's a `dispatch`-side change (auri's lane).
- **Anomaly detection / liveness escalation** in the broker loop — consumes this
  data but is broker work (elodin's lane). See deep doc §5.5, §5.7a.
