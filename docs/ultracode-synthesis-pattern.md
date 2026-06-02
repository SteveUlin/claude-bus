# Ultracode Synthesis Pattern — a final stage with no single point of failure

A drop-in design for the **final synthesis stage** of any `find → verify → synthesize`
workflow. It guarantees that the expensive, verified upstream work survives every
failure of the last stage — rate limits, model outages, synchronous throws, even a
floor that misbehaves. Replace a bare `return await agent(prompt, {schema})` with
`return await hardenedSynthesis(confirmed, { checkpointPath, stats })`.

This doc is the deliverable of a methodology fix; it was **produced by dogfooding
itself** — a workflow whose own final stage is the pattern below designed it, then
adversarial skeptics tried to break it. See [Provenance](#provenance).

## The failure this eliminates (Experiment #1)

A background audit ran 5 finders → 49 verifier votes → **one final synthesizer agent
with no output schema**. The synthesizer hit the Anthropic session rate limit. Because
it had no schema, the runtime accepted the literal banner string
`"You've hit your session limit · resets 11:50am"` as its successful free-text output.
The workflow returned `{report: <banner>, stats}`. The 9 verified findings — the output
of 55 agents and ~2.45M tokens — lived **only** in an in-memory array and the per-subagent
transcript `.jsonl` files. Recovery meant hand-mining transcripts with `jq`.

Two root causes, one compounding the other:

1. **A schema-less final stage turns silent garbage into apparent success.** Nothing
   forced a validated tool call, so any string — including a rate-limit banner — passed.
2. **The verified findings never entered the script's return value.** The one durable
   channel the runtime persists for free was carrying the banner, not the data.

## The load-bearing invariant

> The script **always reaches `return`**, and that `return` **always carries the
> structured findings** as its primary field.

The workflow runtime persists a script's return value to the task-output file and the
completion notification **with no agent and no filesystem involved**. That makes the
return value the *one* durable channel a session-wide rate limit cannot defeat — every
agent-based channel (including a Write-agent checkpoint) fails under a session cap. So
the entire fix reduces to: build a return object with `findings` as its primary field
*before* synthesis, and guarantee the script reaches `return` on every path — including
synchronous throws. Everything else is redundancy or quality.

## Principles

1. **The return value is the only zero-dependency durable channel — make reaching
   `return` with findings attached unconditional.** It needs no agent and no fs, so a
   session cap cannot kill it. Carry `findings` as the primary return field and reach
   `return` on every path.
2. **Schema-constrain every LLM stage so failure is loud (a `null`), never silent
   garbage.** `agent(prompt, {schema})` forces a validated `StructuredOutput` call; a
   rate-limited attempt *fails* instead of returning the banner as success. This `null`
   is the precondition that lets a retry ladder and a floor exist at all.
3. **Schema enforcement lives only inside `agent()` — any non-LLM tier must
   re-validate its own output in plain JS.** The deterministic floor bypasses `agent()`,
   so nothing validates it unless the script does. An unchecked floor revives
   garbage-as-success at the *guaranteed* tier.
4. **`parallel()` nulls failed *agent* thunks only — synchronous JS in the script body
   still throws.** `JSON.stringify` on a BigInt/circular finding, `[...confirmed]` on
   `null`, `f.severity` on a null element, a stripped Node import — each throws past
   `return`. Wrap the whole body in a top-level `try/catch` that *still returns findings*.
5. **Sanitize untrusted upstream data into a guaranteed shape before any throwable
   operation.** `confirmed` comes from a verifier `parallel()` that yields `null` on
   failure, so it may be `null`, `undefined`, or an array containing nulls — exactly when
   a rate limit hits the verifiers. Coerce it to a clean array of plain objects as the
   first statement, so every later spread/sort/map is total.
6. **The on-disk checkpoint is best-effort redundancy — never a gate, never the sole
   durability path.** The script has no fs, so a file write goes through an agent, which
   can itself be rate-limited (and *will* be, under the session cap that caused
   Experiment #1). Never block synthesis on it; never abort when it returns `null`.
7. **Stage finders/verifiers as the cached prefix; keep all synthesis logic after
   them.** `resumeFromRunId` replays the longest unchanged prefix of `agent()` calls. If
   synthesis is the only thing an operator edits on recovery, a re-run replays the
   2.45M-token upstream for free and re-executes only synthesis — turning recovery from
   transcript-mining into a one-line re-run.
8. **Surface degradation honestly via a `tier` field.** A floor result is valid but
   shallow; a failed checkpoint leaves no re-runnable file. Encode the producing tier
   (`opus`/`sonnet`/`haiku`/`floor`/`floor-invalid`/`floor-error`) and a `checkpointed`
   boolean so a consumer can *see* a degraded run instead of trusting it as full
   synthesis. Silent degradation is a softer echo of the original silent-success bug.

## Mechanisms

| Mechanism | What it stops | Tradeoff accepted |
|---|---|---|
| **Findings-as-return-primary + top-level `try/catch` that still returns findings** | The original loss, *and* every synchronous-throw hole (BigInt/circular `stringify`, spread-on-null, stripped imports, floor bugs) that aborts before `return`. | A blanket catch can mask a real logic bug as a `floor-error` success — but a logged, tier-labeled degraded return beats an uncaught throw that loses 2.45M tokens. The `tier` + log make it auditable and `resume`-able. |
| **Schema-constrained synthesis via a single-thunk `parallel()`** | The schema-less SPOF. A bare `await agent(…,{schema})` would *throw* on failure and abort before `return`; inside `parallel()` a failed thunk resolves to `null`, so the script keeps control. | The single-element `parallel()` idiom reads oddly; an author who "simplifies" it to a bare `await` reintroduces throw-aborts. Guard with a comment — the type system can't enforce it. |
| **Cheaper-model retry ladder (`opus→sonnet→haiku`) with a non-empty guard** | Per-model/contention limits knocking out the best model; *and* a rate-limited nudge emitting a schema-valid-but-vacuous `{prioritized:[], summary:''}` recorded as success. | Under a **session-wide** cap every rung fails — the ladder buys nothing and burns wall-clock before the floor. Accepted because the floor + return value still guarantee no loss; keep the ladder short. Handle the legitimately-zero-findings case *before* this stage so the non-empty guard doesn't fall through spuriously. |
| **Deterministic pure-JS floor, re-validated against the schema in-script** | Total LLM unavailability (session cap): synthesis is assembled from already-structured findings with no model call. The in-script re-check stops an off-schema floor from emitting garbage-as-success. | `matchesSynthSchema` is a hand-written subset validator, not a full JSON-Schema engine (no engine is guaranteed without a Node import the sandbox may strip). Keep it adjacent to the schema and update both together. |
| **Best-effort Write-agent checkpoint (redundant on-disk file, never a gate)** | Makes transcript-recovery a *designed file* rather than `jq`-mining, and hands a human an on-disk copy while synthesis grinds. | Under a session cap the haiku Write-agent is also capped → no file. This channel does **not** protect the pre-`return` window during a session cap — only the return value does. Self-reported count is a weak check; acceptable because findings ride the return value regardless. |
| **`resumeFromRunId`-aware staging** | Re-burning the 2.45M-token finder/verifier prefix on recovery. | Only works if the journal survived the crash and the operator keeps the prefix byte-identical. The code cannot enforce that discipline. |

## Anti-patterns

- ✗ **No-schema final stage** (the original SPOF). Always pass `{schema}` so a
  rate-limited attempt fails loudly instead of returning the banner as success.
- ✗ **Findings live only in memory / transcripts.** They must be the *primary field* of
  the return object, built before synthesis is attempted.
- ✗ **Assuming `parallel()` makes the script throw-proof.** It only nulls failed *agent*
  thunks. Sanitize inputs first; wrap the body in a top-level `try/catch`.
- ✗ **Returning the deterministic floor without re-validating it.** `agent()` enforces
  the schema; the floor bypasses `agent()`. Re-check in plain JS, fall back to a minimal
  valid object.
- ✗ **Running the floor (or any author closure) unguarded.** `f.evidence.length` on a
  sparse-but-valid finding throws before `return`. Run the floor inside the `try/catch`;
  pre-sanitize its inputs.
- ✗ **Trusting a cheaper model to dodge a *session-wide* limit.** "haiku is less likely
  to be rate-limited" holds for per-model caps, not the account cap that caused
  Experiment #1 — that caps every model, including the haiku Write-agent.
- ✗ **Gating synthesis on the checkpoint, or aborting when it returns `null`.** It's an
  agent; it can fail. Treat it as best-effort redundancy.
- ✗ **Accepting a vacuous schema-valid synthesis.** Require `prioritized.length > 0`
  before accepting an LLM tier; handle the truly-zero-findings case earlier.
- ✗ **Using `node:crypto` / `fs` / any Node API — or `Date.now()` / `Math.random()` — in
  the script body.** The sandbox strips them; an import or call throws at evaluation and
  kills the stage. Do such work inside an agent, or omit it.
- ✗ **Editing anything upstream of synthesis on a resume.** Any incidental edit to a
  finder/verifier prompt invalidates the cached prefix and re-burns the upstream tokens.
  On recovery, edit *only* the synthesis stage.

## Reference template

Drop-in. Correct against the Workflow mechanics: no fs, no Node API, persistence through
an agent, schema makes failure loud, the return value is the durable store.

> **One correction over the synthesizer's draft:** the original default
> `opts.checkpointPath ?? \`synthesis-checkpoint-${Date.now()}.json\`` calls `Date.now()`,
> which is **forbidden in workflow scripts** (it breaks resume and throws at evaluation).
> It is harmless only because the checklist mandates passing `checkpointPath` explicitly
> (so `??` short-circuits past it) — but a reference template must not ship the landmine.
> The version below uses a static default. Pass a real `checkpointPath` regardless.

```js
// ============================================================================
// hardenedSynthesis — canonical drop-in final stage for ultracode workflows.
// Replace your bare  `return await agent(prompt, {schema})`  final stage with
//   `return await hardenedSynthesis(confirmed, { checkpointPath, stats })`
//
// WORKFLOW MECHANICS THIS RESPECTS (a design that gets these wrong is wrong):
//   * The script has NO fs / NO Node API. We use NO node:crypto, NO fs,
//     NO Date.now()/Math.random() in the script body.
//   * Disk persistence MUST go through a Write-capable agent.
//   * agent(prompt,{schema}) forces+validates StructuredOutput; failure (rate
//     limit / 2 failed nudges) => a bare await THROWS, inside parallel() => null.
//   * The runtime persists the RETURN VALUE durably — IF the script reaches
//     `return`. That is the one channel no agent and no fs can defeat, so a
//     session-wide rate limit (the Experiment #1 trigger) cannot kill it.
//   * parallel() is a barrier that never rejects (failed agent thunk -> null).
//     It does NOT catch synchronous throws in the script body.
//
// LOAD-BEARING INVARIANT: the script ALWAYS reaches `return`, and `return`
// ALWAYS carries the structured findings. A top-level try/catch whose catch
// STILL returns findings makes this hold even against synchronous throws.
// ============================================================================

const SYNTH_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['prioritized', 'summary'],
  properties: {
    summary: { type: 'string' },
    prioritized: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        required: ['id', 'title', 'severity', 'rationale', 'evidenceRefs'],
        properties: {
          id: { type: 'string' },
          title: { type: 'string' },
          severity: { enum: ['critical', 'high', 'medium', 'low'] },
          rationale: { type: 'string' },
          evidenceRefs: { type: 'array', items: { type: 'string' } },
        },
      },
    },
  },
};
const CHECKPOINT_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['written', 'path', 'count'],
  properties: {
    written: { type: 'boolean' },
    path: { type: 'string' },
    count: { type: 'integer' },
  },
};

const SEV = { critical: 0, high: 1, medium: 2, low: 3 };

// Run one fallible LLM stage as a single-thunk barrier: value or null, NEVER
// throws -> the script keeps control. DO NOT refactor to a bare await agent().
async function tryStage(thunk) {
  const [r] = await parallel([thunk]);
  return r ?? null;
}

// Pure-JS floor: no LLM, no fs, no network -> cannot rate-limit. Inputs are
// pre-sanitized to plain objects, so every spread/sort/map is total.
function buildFloor(findings) {
  const prioritized = [...findings]
    .sort((a, b) =>
      (SEV[a.severity] ?? 9) - (SEV[b.severity] ?? 9) ||
      (b.votes ?? 0) - (a.votes ?? 0))
    .map((f) => ({
      id: String(f.id ?? f.title ?? 'unknown'),
      title: String(f.title ?? '(untitled finding)'),
      severity: SEV[f.severity] !== undefined ? f.severity : 'medium', // coerce to enum
      rationale: String(f.summary ?? f.rationale ?? '(auto-assembled)').slice(0, 1000),
      evidenceRefs: Array.isArray(f.refs) ? f.refs.map(String) : [],
    }));
  const crit = prioritized.filter((p) => p.severity === 'critical').length;
  const high = prioritized.filter((p) => p.severity === 'high').length;
  return {
    summary:
      `${prioritized.length} verified findings, ${crit} critical, ${high} high. ` +
      `Deterministic floor — LLM synthesis unavailable (rate limit). Findings ` +
      `complete and ordered by severity; prose narrative absent, regenerate on resume.`,
    prioritized,
  };
}

// In-script schema check: agent() validates LLM tiers, but the floor bypasses
// agent(), so the floor MUST be re-checked here or it can emit silent garbage.
function matchesSynthSchema(o) {
  if (!o || typeof o !== 'object') return false;
  if (typeof o.summary !== 'string' || !Array.isArray(o.prioritized)) return false;
  const SEVS = ['critical', 'high', 'medium', 'low'];
  return o.prioritized.every((p) =>
    p && typeof p === 'object' &&
    typeof p.id === 'string' && typeof p.title === 'string' &&
    SEVS.includes(p.severity) && typeof p.rationale === 'string' &&
    Array.isArray(p.evidenceRefs) && p.evidenceRefs.every((e) => typeof e === 'string'));
}

/**
 * @param {*} confirmed   Verified findings (UNTRUSTED: may be null / contain nulls).
 * @param {object} opts   { checkpointPath?, ladder?, stats? }
 * @returns durable result object (also the workflow's return value).
 */
async function hardenedSynthesis(confirmed, opts = {}) {
  const checkpointPath = opts.checkpointPath ?? 'synthesis-checkpoint.json'; // no Date.now() in scripts
  const ladder = opts.ladder ?? ['opus', 'sonnet', 'haiku'];
  const stats = opts.stats ?? {};

  // SANITIZE FIRST: coerce untrusted upstream into a guaranteed array-of-objects
  // BEFORE any throwable work. Upstream parallel() yields null on verifier
  // failure, so `confirmed` may be null/undefined or contain null elements.
  const findings = Array.isArray(confirmed)
    ? confirmed.filter((x) => x && typeof x === 'object')
    : [];

  // CHANNEL 2 (always-on): build the durable return object NOW, findings primary.
  const out = { findings, synthesis: null, tier: 'pending', checkpointed: false, stats };

  try {
    // CHANNEL 1 (best-effort): durable disk file via Write-agent, BEFORE synthesis.
    // Journaled before synthesis -> on resume this + finders/verifiers are the
    // cached prefix. Under a SESSION cap this haiku agent is also limited -> null;
    // that is fine, CHANNEL 2 carries the data regardless. JSON.stringify is safe
    // here because `findings` is sanitized plain objects (still inside try/catch).
    const checkpointJson = JSON.stringify(findings);
    const ck = await tryStage(() => agent(
      `Use the Write tool to write this JSON VERBATIM to "${checkpointPath}", then ` +
      `call StructuredOutput with {written:true, path:"${checkpointPath}", count:${findings.length}}. ` +
      `Do not edit, summarize, or reformat.\nJSON:\n${checkpointJson}`,
      { schema: CHECKPOINT_SCHEMA, model: 'haiku' },
    ));
    out.checkpointed = !!(ck && ck.written && ck.count === findings.length);
    log({ event: 'checkpoint', ok: out.checkpointed, path: checkpointPath, count: findings.length });

    // SYNTHESIS: schema-constrained, cheaper-model retry ladder. Each rung is a
    // distinct agent() call -> resumeFromRunId can re-run ONLY synthesis.
    const synthPrompt =
      `Merge and PRIORITIZE these VERIFIED findings into a report. Use ONLY the ` +
      `data provided; do not invent findings. Return strictly per schema.\n\n` +
      JSON.stringify(findings);
    for (const model of ladder) {
      const r = await tryStage(() => agent(synthPrompt, { schema: SYNTH_SCHEMA, model }));
      // Reject vacuous-but-valid output (rate-limited nudge emitting empty arrays).
      if (r && Array.isArray(r.prioritized) && r.prioritized.length > 0) {
        out.synthesis = r; out.tier = model;
        log({ event: 'synthesis_ok', tier: model });
        break;
      }
      log({ event: 'synthesis_rung_failed_or_empty', model });
    }

    // DETERMINISTIC FLOOR: pure JS, cannot rate-limit. RE-VALIDATED in-script,
    // because agent() schema enforcement does not run for a non-LLM tier.
    if (!out.synthesis) {
      const floor = buildFloor(findings);
      if (matchesSynthSchema(floor)) {
        out.synthesis = floor; out.tier = 'floor';
        log({ event: 'synthesis_floor_used', count: findings.length });
      } else {
        // Floor itself produced off-schema output: emit a minimal valid object,
        // never off-schema garbage. Findings still ride the return value.
        out.synthesis = { prioritized: [], summary: `Floor produced off-schema output; ${findings.length} findings preserved in return value.` };
        out.tier = 'floor-invalid';
        log({ event: 'synthesis_floor_invalid', count: findings.length });
      }
    }
  } catch (err) {
    // Any SYNCHRONOUS throw (BigInt/circular inside a finding, missing primitive,
    // a floor/closure bug) lands here. parallel() does NOT catch these. We STILL
    // return findings -> the load-bearing invariant holds unconditionally.
    log({ event: 'synthesis_threw', message: String(err && err.message) });
    if (!out.synthesis) {
      out.synthesis = { prioritized: [], summary: 'Synthesis threw; findings preserved in return value.' };
      out.tier = 'floor-error';
    }
  }

  // Reached on EVERY path. `findings` is durable via the runtime's return-value
  // persistence — independent of any agent, the filesystem, and any model limit.
  log({ event: 'synthesis_done', tier: out.tier, checkpointed: out.checkpointed, findings: findings.length });
  return out;
}
```

## Wiring checklist

- ☐ Structure the workflow as **finders → verifiers → hardenedSynthesis**, with
  finders+verifiers as the cached prefix and *all* synthesis logic strictly after them.
- ☐ Pass the verified findings array as the first arg and make
  `return await hardenedSynthesis(confirmed, { checkpointPath, stats })` the workflow's
  final statement — don't post-process its result in a way that could throw before the
  real return.
- ☐ Make downstream consumers branch on `out.tier` (`opus`/`sonnet`/`haiku` = LLM,
  `floor` = degraded, `floor-invalid`/`floor-error`/`pending` = investigate) and on
  `out.checkpointed`.
- ☐ Define `SYNTH_SCHEMA` with `additionalProperties:false` + explicit `required`, and
  keep `matchesSynthSchema` **in lockstep** — update both together.
- ☐ If your finding fields differ from `{id,title,severity,votes,summary,refs}`, adjust
  `buildFloor`'s accessors *and* `matchesSynthSchema` together; keep `buildFloor` pure
  (no agent, no fs, no throw — use `??` defaults and `String()` coercion).
- ☐ Verify `buildFloor` coerces any out-of-enum `severity` to a valid value rather than
  passing it through.
- ☐ Keep the single-thunk `parallel()` idiom in `tryStage`; comment-forbid refactoring
  it to a bare `await agent()`.
- ☐ Set the ladder to your contention profile (default `['opus','sonnet','haiku']`);
  keep it short so the failing-retry wall-clock window before the floor stays bounded.
- ☐ Handle the legitimately-zero-findings case **before** calling `hardenedSynthesis`,
  so a real empty result isn't forced to the floor.
- ☐ Unit-test `hardenedSynthesis` with: (a) `null` confirmed, (b) `[null, {...}]`,
  (c) a finding with out-of-enum `severity:'blocker'`, (d) a finding whose field is a
  BigInt — assert each returns an object with `findings` populated and a schema-valid
  `synthesis`.
- ☐ On a real rate-limit, recover by re-running with `resumeFromRunId=<runId>`, editing
  **only** the synthesis stage; confirm finders/verifiers/checkpoint replay from the
  journal before relying on it.
- ☐ Point `checkpointPath` at a readable path; treat the file as convenience and the
  return value as the guarantee.

## Residual risks accepted

- **The irreducible window** between "finders/verifiers done" and "script reaches
  `return`." The script has no fs, so the only copy that survives a session-wide cap is
  the return value, persisted at `return`. If the process dies (OOM, lid-close/suspend,
  reboot, deploy, operator `Ctrl-C` on a visibly-hung ladder) *after* findings are in
  memory but *before* the runtime serializes the return, only the subagent transcripts +
  resume journal survive. We shrink the window (sanitize-then-build-return-object first,
  short ladder, best-effort checkpoint) but the mechanics forbid closing it from the
  script. **This is the single point where Experiment #1 can still recur** — narrowed
  from "any rate limit" to "process death during the synthesis window concurrent with a
  session-wide limit."
- Under a session-wide cap the Write-agent checkpoint is also capped → no on-disk file.
  The return value still carries findings, so this loses the convenience file, not the
  data (unless combined with the mid-window death above).
- `resumeFromRunId` recovery assumes the journal flushed before the crash and the
  operator keeps the prefix byte-identical; the code can't enforce this.
- The floor is structurally valid but **semantically shallow** — severity+votes ordering
  and a templated summary, no cross-finding reasoning or dedup. `tier='floor'` means no
  data loss and valid shape, *not* a good report; re-synthesize via resume once limits
  clear.
- `matchesSynthSchema` is a hand-written subset validator and can drift from the schema;
  keep them adjacent and update together.
- Large finding sets inline a lot of input tokens into the checkpoint + synthesis
  prompts; at extreme scale switch synthesis to read the checkpoint file via an agent
  (re-introducing a dependency on the checkpoint having landed) — a documented crossover.
- The top-level catch can mask a genuine logic bug as a `floor-error` success. Accepted:
  a logged, tier-labeled degraded return beats an uncaught throw that loses the work.
- **Garbage-in:** this pattern hardens the *synthesis* stage and assumes the verifier
  stage is itself schema-hardened. It faithfully preserves whatever `confirmed` contains.

## Provenance

This pattern was produced by a workflow whose own final stage was the hardened design
— eating its own dog food. **3 design agents** (robustness / frugality / reusability
angles) each proposed a hardening design; **6 adversarial skeptics** (2 per design) tried
to break each one; a checkpoint agent persisted the structured designs+attacks to disk
*before* synthesis; the hardened synthesis stage (schema + `opus→sonnet→haiku` ladder +
deterministic floor + return-value/file checkpoint) merged the survivors. 11 agents,
~498K tokens, ~12 min. Synthesis succeeded on the first rung with the ladder and floor
standing ready as the proof-of-structure.

The adversarial stage earned its cost: **all three designs were broken** in identical
ways — a synchronous throw before `return` (`JSON.stringify` on a BigInt/circular
finding, `[...confirmed]` on null), a `node:crypto` import the sandbox would strip, an
unvalidated floor emitting off-schema output, and a vacuous schema-valid `{prioritized:[]}`
recorded as success. Every one of those holes is closed in the canonical template above.
Lineage: Experiment #1 (broker reliability audit) → logged as harness gap #18.
