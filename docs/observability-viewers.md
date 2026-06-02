# Observability viewers — design + state (kvothe lane)

Author: kvothe · 2026-06-02 · For: auri's max-parallelism observability dispatch
Status: MODEL column landed; token-rate + verify-viewer contracts defined, builds pending.

This doc is the **durable anchor** for the observability viewer work so it
survives a `/clear` (the #10 lesson: chat-only design evaporates — bast lost
SEC-1 twice that way). Fresh-me should be able to BUILD from this without the
originating conversation.

## The architectural pattern (read first)

`$STATE/status/<agent>.json` is the contract surface. The **broker's
`maybeScanTokens`** (`src/delivery.cpp`) is the SINGLE producer — it tails each
live agent's transcript, computes occupancy from the last assistant turn, and
writes the file. **I own the schema; I render it in `bus monitor`. Viewers are
ephemeral consumers (multiple, can die — see [[long-running-viewers-die]]) and
must NEVER be the producer of durable/recovery-critical state.** This is why
MODEL and token-rate are broker emits, not viewer computations.

### `$STATE/status/<agent>.json` schema

Current (shipped):
```json
{"agent":"kvothe","ts":1780361936834,
 "context_window":{"used_percentage":51,"context_window_size":1000000}}
```
Proposed additions (elodin emits from `maybeScanTokens` — both 1-liners, values
already in hand):
- `"model": "<string>"` — from the transcript turn's `message.model`, skipping
  `<synthetic>`. Lights up the MODEL column.
- `"context_tokens": <int>` — the RAW input-token occupancy (= `last_tokens`,
  input + cache_creation + cache_read, un-rounded). Paired with the existing
  `ts`, any consumer derives Δtokens/Δt. RAW (not a baked rate) so P2 picks its
  own window AND dodges the integer-`used_percentage` coarseness: at a 1M window
  1% = 10k tokens, so a slow-but-working agent reads as 0% delta = a FALSE
  "token-rate≈0 stuck".

## Item 1 — token/context monitor ("models + ctx-sizes", sulin's nudge)

Goal: each agent's MODEL + CONTEXT-FILL% at a glance. Context-fill is THE
reliability signal — agents degrade + give confident-false reports near ~100%
(hit repeatedly: sim/mola/elodin). Feeds P3 context-watchdog.

State:
- **CTX% column — DONE (shipped).** `bus monitor` shows `pct%/size` with danger
  tiers (≥90 red, ≥75 yellow) sourced from `$STATE/status`. The core ask exists.
- **MODEL column — COMMITTED (`3450a2a5`, this workspace, not pushed).**
  `sub_monitor.cpp`: `contextStatsFor` reads top-level `"model"`; `formatModel`
  abbreviates (`claude-opus-4-8` → `opus-4-8`); placed between AGE and CTX. Shows
  `—` until elodin emits `model`. Push when comms opens a window + the emit lands.
- **effort — OUT OF SCOPE.** Statusline-only field; uncaptured since the
  status-decouple. Would need a new producer.
- **Optional follow-up:** a token-rate / `$/turn` monitor column (consume
  `context_tokens` deltas across the monitor's 1Hz ticks) — distinguishes
  occupancy from cost (an agent at 30% doing huge fresh-input turns is expensive;
  95% all-cache-read is cheap).

## Item 2 — P5 verify-side viewer (claimed-done vs artifact-present)

Goal: catch FALSE done-claims — the verify-before-trust signal. Real failures
this fleet hit: bast "SEC-1 landed" but not on disk; sim "RNG built+verified" but
workspace empty.

**Claimed-done is an AGENT-STAMPED DURABLE record — NOT orchestrator-tracked**
(auri's ruling). Principle: if the orchestrator records the claim, the signal
inherits the orchestrator's failure modes (/clear, corruption, missed relay) —
the exact unreliability P5 exists to catch. Self-defeating. Same principle as
commit-early: bast's claim evaporated on /clear *because it was chat-only*. So
the claim must be a structured durable artifact, never chat free-text.

**Schema (I own):**
```json
{"agent":"<name>","task":"<what>","claimed_artifact":"<commit|path|test>","ts":<ms>}
```

**WRITE mechanism — decide WITH elodin (the auto-check is broker-side):**
- **W1 — thin `bus done` CLI** → writes `$STATE/done/<agent>.json` (or appends).
  Simple, viewer reads it directly. No broker change.
- **W2 — route the claim through the broker as a topic** (e.g. `completions`) so
  the broker auto-checks artifact-exists + escalates a mismatch — folds into
  elodin's P2 **R6**. Richer (auto-verify + escalate) but broker-side.
- I own the record schema + the viewer either way.

**Viewer (I build):** a column/card — claimed-done vs artifact-present.
`artifact-present` = read `jj log` in `.workspaces/<agent>` (does
`claimed_artifact` exist as a commit / file / passing test?). **Flag
claimed-but-absent** — that's the false-claim alarm.

Once schema + write land, auri rolls out "agents run `bus done` on completion"
fleet-wide.

## Open handoffs / next actions (resume here post-/clear)

1. **elodin** — emit `"model"` + `"context_tokens"` into `$STATE/status` from
   `maybeScanTokens` (two 1-liners). Lights up MODEL + feeds P2's token-rate.
2. **elodin + me** — pick the claimed-done write mechanism (W1 `bus done` →
   `$STATE/done` vs W2 broker `completions` topic + auto-check). Auto-check is
   broker-side, so elodin co-decides.
3. **me (build)** — the P5 verify-viewer against the chosen mechanism (claimed
   vs `jj log` artifact, flag absent). Optionally the token-rate monitor column.
4. **me (land)** — push MODEL column `3450a2a5` when comms opens a window.
5. **auri** — roll out `bus done` usage once it lands.

## Pointers
- Monitor render + `$STATE/status` read: `src/sub/sub_monitor.cpp`
  (`contextStatsFor`, `formatModel`, `formatCtx`, the header/data `println`).
- Producer: `src/delivery.cpp` `maybeScanTokens` (~line 1283) — `last_tokens`,
  the status write (~1371). See [[project-monitor-column-sources]],
  [[project-off-tty-delivery]], [[reference-claude-code-token-data-sources]].
