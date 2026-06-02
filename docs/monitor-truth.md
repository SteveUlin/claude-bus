# monitor-truth — real context window + live effort (kvothe lane)

Author: kvothe · 2026-06-02 · For: sulin's "the monitor must not lie" mandate
Status: **LIVE-VERIFIED post-relaunch (2026-06-02).** All fleet agents emit
real captures to `$STATE/statusline/<agent>.json`; the monitor renders real
per-model window (1M, not the 200k constant), real tokens/pct, and real
varying effort (observed xhigh / high / low across the fleet). The
effort-live question is SETTLED: effort is observable and rendered — NOT a
gap. Producer v1 (statusline wrapper) deployed via config materialization on
relaunch; monitor consumer via D4 auto-rebuild. sulin RULING: PATH 1 ("a
state file sounds good"). See [[monitor_truth]] memory + docs/p3-trigger-feed.md.

Durable anchor (survives /clear).

## What sulin asked for

Three directives, one package — the monitor's context + effort columns must
show the TRUTH from authoritative sources:

1. **ctx_fill = actual tokens / the REAL per-model context window** as the
   denominator, keyed off the model. NOT a hardcoded 200k. The 200k is a
   *behavioral compact policy*, rendered ONLY as an overlaid threshold
   MARKER ("compact past here"), never as the scale (a 200k denominator
   pegs a 540k-context agent over 100% and lies).
2. **Source mandate**: real token count + real window from the AUTHORITATIVE
   source — certify exactly where each number is read.
3. **EFFORT column** (low/medium/high/xhigh/max) alongside MODEL, same
   source rule. `/effort` changes it live in-pane, so a launch flag goes
   stale — source the CURRENT value. If live effort is NOT observable,
   SAY SO (a gap to surface, never synthesize).

## Source certification (the deliverable for directive 2)

| Number | Authoritative source | Notes |
| --- | --- | --- |
| live tokens | statusline JSON `.context_window.total_input_tokens` | == broker's transcript sum (input+cache_create+cache_read). |
| **real window** | statusline JSON `.context_window.context_window_size` | The ONLY client-side source. 200k vs 1M, computed by Claude Code per-model (incl 1M-beta). CONFIRMED: sulin's `statusline-command.sh:116` reads this exact path. NOT in the transcript. |
| used % | statusline JSON `.context_window.used_percentage` | Claude's own calc against the real window. |
| **effort** | statusline JSON `.effort.level` | Docs: "reflects live `/effort` changes." Launch `--effort` (agent-launch:375) goes stale. NOT in transcript/settings. Doc-certified; empirically TBD — extract defensively, surface as a gap if the live field is empty. |
| model | statusline JSON `.model.id` / `.model.display_name` | |

**The crux:** real window + effort exist in Claude Code ONLY on the
statusline command's stdin — not the transcript, not any file or API. That
is a **Claude Code surface-area GAP, not our design choice.** The broker's
token-scan reads the transcript, so it structurally CANNOT see window/effort
and falls back to `CLAUDE_BUS_CTX_WINDOW` — a fleet constant (layout=1M;
the live broker dropped the env on a manual restart → currently denominating
200k → that's the 100%-peg bug). A fleet constant is exactly what the
mandate forbids.

## Design — get the value OUTSIDE the render path, into a decoupled FILE

This is NOT embracing statusline-coupling. It is the cleanest available way
to tap a value Claude Code exposes only through the statusline render path,
and land it in a plain decoupled file the monitor reads. (sulin was explicit
about that framing.)

- **Producer v1** — `settings/hooks-shared/statusline.sh`: a project-scoped
  wrapper on the *agents'* `statusLine` (settings/claude-settings.json).
  Tees stdin → writes `$STATE/statusline/<agent>.json` (atomic tmp+rename)
  → delegates to sulin's renderer so the in-pane bar is unchanged. Capture
  is best-effort; nothing can break the render. Project-scoped, so sulin's
  own (non-bus) sessions are untouched (their user `statusLine` still wins).
  Keyed by `$CLAUDE_BUS_AGENT_ID` (exported at agent-launch:388).
- **The FILE is the stable contract; the producer can change.**
  `$STATE/statusline/<agent>.json` is the low-regret contract. effort /
  tokens / model *could* later be sourced from an OTel-derived writer
  (producer v2) with the monitor read path NEVER changing — but
  `context_window_size` cannot (OTel doesn't export it; statusline-only), so
  a window producer is permanent. Keep the wrapper minimal regardless; its
  irreplaceable job is the window. See the OTel checkpoint below.

### The contract — `$STATE/statusline/<agent>.json` (PRODUCER-AGNOSTIC)

This file IS the interface. Any producer — the wrapper-tee (v1) or a future
OTel-derived writer (v2) — MUST satisfy this exactly; the monitor read path
never changes. OTel is the intended backbone; this contract is what makes
the v1→v2 swap cheap.

```json
{ "agent": "kvothe", "ts": 1780380200000, "model_id": "claude-opus-4-8",
  "model_display": "Opus 4.8", "context_window_size": 1000000,
  "total_input_tokens": 53213, "used_percentage": 5,
  "effort_level": "high", "exceeds_200k": false }
```

| Field | Type | Req | Semantics / consumer use |
| --- | --- | --- | --- |
| `agent` | string | yes | Agent name (== filename stem). |
| `ts` | int (ms epoch) | yes | When captured. For future staleness gating (not yet enforced). |
| `model_id` | string | yes | → MODEL cell (`claude-` prefix stripped). |
| `model_display` | string | no | Carried for contract; not yet rendered. |
| `context_window_size` | int | **yes** | → CTX denominator. **AUTHORITATIVE-GATE: a record is honored only when this is > 0**; else the consumer falls back to the broker's `$STATE/status`. A v2 producer that can't supply a real window MUST omit the record, not write 0. |
| `total_input_tokens` | int | yes | → drives the 200k POLICY color marker (red ≥200k, yellow ≥170k). |
| `used_percentage` | int | yes | → CTX pct (`25%/1M`). Against the real window. |
| `effort_level` | string | yes | → EFFORT cell. Real live field (reflects `/effort`). `""` ⇒ `—` = not-yet-captured / model-without-effort — NOT a gap, never the launch flag. |
| `exceeds_200k` | bool | no | Carried for contract; not yet rendered. |

Producer obligations: **atomic write** (tmp + rename — concurrent reader);
one JSON object per file; overwrite each update. Consumer obligations:
prefer this file, fall back to broker `$STATE/status` when absent or
gate-failed. A v2 (OTel) writer targeting these eight fields drops in with
zero monitor change.

- **Consumer** — `src/sub/sub_monitor.cpp` `contextStatsFor`: prefers the
  statusline file (real window + effort), falls back to the broker
  `$STATE/status` (knob window, no effort) when the capture is absent
  (pre-rollout pane). CTX = `<pct>%/<real window>`; the **200k policy** is a
  color marker keyed to the *token count* (red past 200k, yellow ≥170k) so
  "past the compact line" stays salient even at 25%/1M — the marker, never
  the denominator. EFFORT column shows the live level or `—`.

## OTel checkpoint — RESOLVED: OTel cannot retire the wrapper

Answered authoritatively (sulin via claude-code-guide, 2026-06-02 — no
elodin checkpoint needed): **OTel emits effort + tokens + cost + model, but
does NOT export `context_window_size`.** The true per-session window
(200k-vs-1M) is exposed by Claude Code ONLY via statusline stdin — not the
transcript, not OTel.

So the wrapper **STAYS**, narrowed to its one irreplaceable job: capturing
`context_window_size`. The other fields (effort/tokens/model) *could* later
be sourced from an OTel-derived writer — but the window cannot, so a window
producer is permanent unless we accept a fleet CONSTANT instead (sulin
sub-decision; auri folding it into the OTel fork). The
`$STATE/statusline/<agent>.json` contract is unchanged either way; this
wrapper keeps producing the full file as the single producer v1.

## Next increment — STOP PANE-READING (kvothe champions the monitor side)

sulin elevated "stop zellij pane-reading" to a first-class goal across the
observability pillar. This wrapper is **step 1**: ctx/window/effort move off
any pane-read onto a file. After it lands:

1. **AUDIT** where the monitor pipeline still depends on pane scraping
   (`dump-screen`) for state (buffer/draft/attach/mode), vs. file/RPC.
2. **MIGRATE** those to file/RPC; make pane-reading the FALLBACK, not the
   primary.
3. Broker-side pane-reads (delivery readiness) are **elodin's** lane; I lead
   the monitor side + coordinate the cross-cutting goal.

## Status / open

1. ~~certify sources~~ — DONE (table above).
2. ~~producer v1 wrapper + file schema~~ — DONE (`statusline.sh`); handed
   shape to bast for his verification harness.
3. ~~monitor consumer (real window + policy marker + EFFORT)~~ — DONE,
   verified on a seeded frame (`25%/1M`, effort `high`, policy red past 200k).
4. **LAND + relaunch** to deploy (config materialization + the new hook take
   effect on agent relaunch; viewer code via D4 auto-rebuild — no broker
   restart).
5. ~~effort-live: confirm `.effort.level` populates on real JSON~~ — DONE.
   Post-relaunch all agents emit it; observed xhigh/high/low. NOT a gap.
6. OTel checkpoint (4) + stop-pane-reading audit (next increment).
