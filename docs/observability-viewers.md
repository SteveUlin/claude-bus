# Observability viewers — design + state (kvothe lane)

Author: kvothe · 2026-06-02 · For: auri's max-parallelism observability dispatch
Status: MODEL column committed; P5 verify-viewer (`bus done` + `bus verify`)
BUILT + committed (95d22533); token-rate column pending elodin's emit on main.

> **STEWARDSHIP UPDATE (kvothe, 2026-06-03)** — reconciling this anchor with
> landed reality (auri handed me ownership; doc held open while tails close):
> - **EFFORT is now LIVE, no longer out-of-scope.** The monitor-truth build added
>   a SECOND producer — a project-scoped statusline WRAPPER that writes
>   `$STATE/statusline/<agent>.json` with the real per-model window **and live
>   `.effort.level`**. The EFFORT column shipped (`sub_monitor.cpp`); §Item-1's
>   "effort — OUT OF SCOPE" below is SUPERSEDED. See [[project-monitor-truth]].
> - **The producer story is now TWO sources, not one:** (1) statusline-wrapper →
>   `$STATE/statusline/<agent>.json` = AUTHORITATIVE window + effort; (2) broker
>   `maybeScanTokens` → `$STATE/status/<agent>.json` = token occupancy + the
>   FALLBACK window (knob, no effort) when the capture is absent. The
>   single-producer/ephemeral-consumer invariant still holds PER FILE.
> - **Still-open tails (why this doc lives):** elodin's `model`+`context_tokens`
>   emit (post-P2), the optional token-rate column, auri's `bus done` fleet
>   rollout. Archive/kill when these close or fold into the Phase-2 observability
>   regeneration.

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

**WRITE mechanism — SETTLED with elodin: W1.** `bus done "<task>"
"<artifact>"` appends an agent-stamped line to `$STATE/done/<agent>.jsonl`.
Broker-free write (I own it); elodin's P2 **R6** auto-check hooks the same
dir as a pure READER — additive, zero producer coupling. W2 (claim-as-
broker-topic) was rejected: it would make the broker the *producer* of the
claim, the exact coupling that lets the signal inherit the orchestrator's
failure modes. (Forward note from elodin: R6's on-disk verify wants
`claimed_artifact` to be a stat-able path; my viewer already dual-probes
path-OR-commit, so no schema change — add an optional `artifact_path` field
only if R6 later needs it.)

**Both surfaces BUILT + committed (95d22533):**
- `bus done` → `src/sub/sub_produce.cpp` (`subDone`). Schema
  `{agent,task,claimed_artifact,ts}`, append-only JSONL.
- `bus verify` → `src/sub/sub_verify.cpp`. Reads `$STATE/done/*.jsonl`;
  per claim, artifact-present = filesystem path exists OR
  `jj log -r <artifact>` resolves in the agent's workspace
  (`--ignore-working-copy`, never snapshots a peer tree). **Flags
  claimed-but-absent** = the false-claim alarm; exit 1 on any miss.
  One-shot (the jj/fs probes are too expensive for a 1-Hz loop, and
  one-shot can't silently die mid-frame — [[long-running-viewers-die]]).
  Tested: real commit ✓, real path ✓, bogus ref ✗ MISSING, `test:` ? manual.

Remaining: auri rolls out "agents run `bus done` on completion" fleet-wide
(role-prompt line or Stop-hook — held off self-stamping so the writer isn't
lone). Cosmetic polish deferred: the multibyte status glyphs (✓/✗/?) make
`{:<11}` pad by bytes not display width, so the STATUS column drifts ~2 cols.

## Open handoffs / next actions (resume here post-/clear)

1. **elodin** — emit `"model"` + `"context_tokens"` into `$STATE/status` from
   `maybeScanTokens`. DONE on elodin's side (built+verified `context_tokens`
   + `model`); lands on main right after P2 Phase A. MODEL column lights up
   the moment it's on main + the broker's rebuilt.
2. ~~W1 vs W2~~ — SETTLED: W1. (done)
3. ~~P5 verify-viewer build~~ — DONE (95d22533: `bus done` + `bus verify`).
4. ~~me (land)~~ — DONE. MODEL + doc + verify-viewer stack LANDED on
   main@origin (commit 1bf0438f). Live fleet-wide after the next relaunch.
5. **rate-routing — SETTLED: option (c), NO write-back.** Do NOT write a
   rate field into `$STATE/status` — elodin owns that file (atomic-rename
   full-overwrite every ~5s); a 2nd writer = lost updates (he caught this).
   The rate is a window-dependent derivation, not a shareable artifact, so:
   elodin stays SINGLE producer of `$STATE/status` (raw `context_tokens`
   +`ts`+`model`); each consumer derives its OWN rate. P2 R3/R4 derives its
   detection rate internally from the token history `maybeScanTokens`
   already keeps (its own smoothed window) — that's elodin's Phase B/C, not
   mine. The token-rate MONITOR column is now an OPTIONAL human-display
   follow-up (its P2 consumer self-serves) — derive Δtokens/Δt in-memory
   across the monitor's 1Hz ticks (track last-seen `ts`, recompute only when
   elodin's ~5s emit bumps it), render like MODEL (`—` until present). Build
   only if pulled. Fallback if P2 ever needs the human's EXACT number:
   separate `$STATE/token-rate/<agent>.json` I own, elodin reads.
6. **auri** — roll out `bus done` fleet-wide: a ROLE-PROMPT convention line,
   NOT a Stop-hook (the claim is agent-authored semantic knowledge only the
   agent has; a Stop-hook knows neither task nor artifact). Sequence: I
   land (done) → relaunch so binaries have the verbs → I wire the
   completion-protocol line into shared agent guidance (I own the surface).

## Pointers
- Monitor render + `$STATE/status` read: `src/sub/sub_monitor.cpp`
  (`contextStatsFor`, `formatModel`, `formatCtx`, the header/data `println`).
- Producer: `src/delivery.cpp` `maybeScanTokens` (~line 1283) — `last_tokens`,
  the status write (~1371). See [[project-monitor-column-sources]],
  [[project-off-tty-delivery]], [[reference-claude-code-token-data-sources]].
