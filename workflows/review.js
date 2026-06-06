export const meta = {
  name: 'claude-bus-review',
  description: 'Multi-agent review + comprehension of claude-bus by area x lens, adversarially verified (majority-refute kills), hardened synthesis.',
  whenToUse: 'Re-run on a diff or major checkpoint. args.mode=review => critique + cleanup findings (APPLY-NOW/PROPOSE/REFUTED). args.mode=map => handoff artifacts (system map, per-area briefs, reading order).',
  phases: [
    { title: 'Find' },
    { title: 'Verify' },
    { title: 'Synthesize' },
    { title: 'Explain' },
  ],
};

// ============================================================================
// claude-bus review/comprehension workflow.
//
// READ-ONLY BY DESIGN. Finders, verifiers, and explainers only READ; synthesis
// only assembles. The workflow NEVER writes source. Confirmed APPLY-NOW
// cleanups are applied by chronicler afterward, sequentially, in a jj workspace
// (NOT git-worktree — Workflow's isolation:'worktree' would fight the jj house).
// Keeping the fan-out read-only sidesteps that tension and makes re-runs cheap.
//
// args: { mode?: 'review' | 'map', areas?: string[] }  // areas subsets AREA_MAP
// ============================================================================

// ---- The AREA MAP: the single source of truth both modes share. -----------
// STEER POINT: this table is what auri/sulin tune as the code evolves. Files
// are read by the finder/explainer; `lenses` selects what to hunt; `docs` are
// the design docs that define the area's invariants (so a finder can tell
// "violates the design" from "is the design"); `heavy` arms a loop-until-dry
// tail (the new code the audit targeted).
const AREA_MAP = [
  {
    key: 'kernel',
    title: 'Kernel substrate',
    files: 'src/topic_log.{cpp,h} src/topic_registry.{cpp,h} src/json_min.{cpp,h} src/event.{cpp,h}',
    lenses: ['correctness', 'concurrency', 'invariant'],
    docs: ['docs/broker-spec.md', 'docs/broker-seam-redesign.md'],
    heavy: false,
  },
  {
    key: 'delivery',
    title: 'Delivery loop',
    files: 'src/delivery.cpp src/delivery.h',
    lenses: ['correctness', 'concurrency', 'resource', 'invariant'],
    docs: ['docs/broker-spec.md', 'docs/policy-actors.md'],
    heavy: true,
  },
  {
    key: 'broker',
    title: 'Broker daemon',
    files: 'src/broker.{cpp,h} src/signals.h src/state_paths.h src/retention.h',
    lenses: ['concurrency', 'invariant', 'resource'],
    docs: ['docs/broker-spec.md', 'docs/broker-intake-decouple.md'],
    heavy: false,
  },
  {
    key: 'rpc',
    title: 'RPC / wire / dispatch',
    files: 'src/rpc.{cpp,h} src/dispatch.{cpp,h}',
    lenses: ['correctness', 'concurrency'],
    docs: ['docs/broker-intake-decouple.md'],
    heavy: true,
  },
  {
    key: 'policy',
    title: 'Policy actors',
    files: 'src/policy.{cpp,h} src/recovery_actor.{cpp,h} src/dispatch_actor.{cpp,h} src/blackboard_actor.{cpp,h} src/recovery.{cpp,h}',
    lenses: ['correctness', 'invariant'],
    docs: ['docs/policy-actors.md', 'docs/work-queue-dispatch.md', 'docs/broker-auto-recovery.md'],
    heavy: true,
  },
  {
    key: 'readers',
    title: 'Readers / derivation',
    files: 'src/agent_status.{cpp,h} src/task_model.{cpp,h} src/trigger_feed.{cpp,h} src/pane_state.h src/tail_reader.h',
    lenses: ['correctness', 'invariant'],
    docs: ['docs/broker-seam-redesign.md', 'docs/observability-viewers.md'],
    heavy: false,
  },
  {
    key: 'pane',
    title: 'Pane I/O',
    files: 'src/pane.{cpp,h} src/tty_policy.h',
    lenses: ['correctness', 'concurrency'],
    docs: ['docs/pane-read-audit.md', 'docs/broker-spec.md'],
    heavy: false,
  },
  {
    key: 'cli',
    title: 'CLI surface',
    files: 'src/sub/*.cpp src/bus.{cpp,h} src/bus_main.cpp src/sub.h',
    lenses: ['correctness', 'simplification'],
    docs: ['docs/bus-commands.md'],
    heavy: false,
  },
];

const LENS_GUIDE = {
  correctness:
    'logic bugs, off-by-one, cursor/ack/epoch invariants, edge + error paths, ' +
    'misuse of return values, unchecked syscalls',
  concurrency:
    'races, partial/torn file writes, fd + signal handling, EINTR, ' +
    'suspend/clock-jump (wall vs mono), write atomicity, accept()/poll wedges',
  resource:
    'fd leaks, unbounded per-agent map growth, the dead-agent GC story, ' +
    'memory ownership, files left open on error paths',
  invariant:
    'does the code PRESERVE the documented design? kernel triad (append-log + ' +
    'cursor-advances-on-ack-only + boot-epoch), Policy no-cursor-verb, the ' +
    'link-boundary guards, escalate-once. Cite the doc you checked against.',
  simplification:
    'vestigial/dead code, over-abstraction, duplication, dead branches. ' +
    'Flag ONLY with high confidence; deliberate escape-hatches are NOT dead.',
};

// ---- Schemas (every LLM stage is schema-constrained; failure is loud). -----
const FINDINGS_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['area', 'findings'],
  properties: {
    area: { type: 'string' },
    findings: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        required: ['id', 'title', 'file', 'line', 'lens', 'severity', 'claim',
          'why_it_matters', 'suggested_fix', 'confidence'],
        properties: {
          id: { type: 'string' },
          title: { type: 'string' },
          file: { type: 'string' },
          line: { type: 'integer' },
          lens: { type: 'string' },
          severity: { enum: ['critical', 'high', 'medium', 'low'] },
          claim: { type: 'string' },
          why_it_matters: { type: 'string' },
          suggested_fix: { type: 'string' },
          confidence: { enum: ['high', 'medium', 'low'] },
        },
      },
    },
  },
};

const VERDICT_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['refuted', 'reason'],
  properties: {
    refuted: { type: 'boolean' },
    reason: { type: 'string' },
  },
};

const EXPLAIN_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['area', 'what', 'why_shaped_this_way', 'why_valuable',
    'entry_points', 'intra_area_reading_order', 'design_tensions'],
  properties: {
    area: { type: 'string' },
    what: { type: 'string' },
    why_shaped_this_way: { type: 'string' },
    why_valuable: { type: 'string' },        // what regresses without it
    entry_points: { type: 'array', items: { type: 'string' } },
    intra_area_reading_order: { type: 'array', items: { type: 'string' } },
    design_tensions: { type: 'array', items: { type: 'string' } },
  },
};

// ===========================================================================
// Hardened synthesis (docs/ultracode-synthesis-pattern.md) — verbatim drop-in.
// The return value is the one durable channel; the script ALWAYS reaches
// `return` carrying findings, even on a synchronous throw.
// ===========================================================================
const SYNTH_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['prioritized', 'summary'],
  properties: {
    summary: { type: 'string' },
    prioritized: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        required: ['id', 'title', 'severity', 'bucket', 'rationale', 'evidenceRefs'],
        properties: {
          id: { type: 'string' },
          title: { type: 'string' },
          severity: { enum: ['critical', 'high', 'medium', 'low'] },
          bucket: { enum: ['APPLY-NOW', 'PROPOSE'] },
          rationale: { type: 'string' },
          evidenceRefs: { type: 'array', items: { type: 'string' } },
        },
      },
    },
  },
};
const SEV = { critical: 0, high: 1, medium: 2, low: 3 };

async function tryStage(thunk) {
  const [r] = await parallel([thunk]);
  return r ?? null;
}

function buildFloor(findings) {
  const prioritized = [...findings]
    .sort((a, b) =>
      (SEV[a.severity] ?? 9) - (SEV[b.severity] ?? 9) ||
      (b.votes ?? 0) - (a.votes ?? 0))
    .map((f) => ({
      id: String(f.id ?? f.title ?? 'unknown'),
      title: String(f.title ?? '(untitled finding)'),
      severity: SEV[f.severity] !== undefined ? f.severity : 'medium',
      bucket: f.bucket === 'APPLY-NOW' ? 'APPLY-NOW' : 'PROPOSE',
      rationale: String(f.claim ?? f.rationale ?? '(auto-assembled)').slice(0, 1000),
      evidenceRefs: [String(f.file ?? '') + ':' + String(f.line ?? '')],
    }));
  const crit = prioritized.filter((p) => p.severity === 'critical').length;
  const high = prioritized.filter((p) => p.severity === 'high').length;
  return {
    summary:
      `${prioritized.length} verified findings, ${crit} critical, ${high} high. ` +
      `Deterministic floor — LLM synthesis unavailable. Findings complete and ` +
      `ordered by severity; regenerate prose on resume.`,
    prioritized,
  };
}

function matchesSynthSchema(o) {
  if (!o || typeof o !== 'object') return false;
  if (typeof o.summary !== 'string' || !Array.isArray(o.prioritized)) return false;
  const SEVS = ['critical', 'high', 'medium', 'low'];
  const BKT = ['APPLY-NOW', 'PROPOSE'];
  return o.prioritized.every((p) =>
    p && typeof p === 'object' &&
    typeof p.id === 'string' && typeof p.title === 'string' &&
    SEVS.includes(p.severity) && BKT.includes(p.bucket) &&
    typeof p.rationale === 'string' &&
    Array.isArray(p.evidenceRefs) && p.evidenceRefs.every((e) => typeof e === 'string'));
}

async function hardenedSynthesis(confirmed, opts = {}) {
  const checkpointPath = opts.checkpointPath ?? 'review-synthesis-checkpoint.json';
  const ladder = opts.ladder ?? ['opus', 'sonnet', 'haiku'];
  const stats = opts.stats ?? {};

  const findings = Array.isArray(confirmed)
    ? confirmed.filter((x) => x && typeof x === 'object')
    : [];

  const out = { findings, synthesis: null, tier: 'pending', checkpointed: false, stats };

  try {
    const checkpointJson = JSON.stringify(findings);
    const ck = await tryStage(() => agent(
      `Use the Write tool to write this JSON VERBATIM to "${checkpointPath}", then ` +
      `call StructuredOutput with {written:true, path:"${checkpointPath}", count:${findings.length}}. ` +
      `Do not edit, summarize, or reformat.\nJSON:\n${checkpointJson}`,
      { schema: { type: 'object', additionalProperties: false,
          required: ['written', 'path', 'count'],
          properties: { written: { type: 'boolean' }, path: { type: 'string' },
            count: { type: 'integer' } } },
        model: 'haiku' },
    ));
    out.checkpointed = !!(ck && ck.written && ck.count === findings.length);
    log({ event: 'checkpoint', ok: out.checkpointed, count: findings.length });

    const synthPrompt =
      `Merge and PRIORITIZE these VERIFIED claude-bus review findings into a report. ` +
      `Use ONLY the data provided; do not invent findings. Assign each a bucket: ` +
      `"APPLY-NOW" only if high-confidence AND low-risk AND surgical; otherwise ` +
      `"PROPOSE" (anything design-touching or uncertain). Return strictly per schema.\n\n` +
      JSON.stringify(findings);
    for (const model of ladder) {
      const r = await tryStage(() => agent(synthPrompt, { schema: SYNTH_SCHEMA, model }));
      if (r && Array.isArray(r.prioritized) && r.prioritized.length > 0) {
        out.synthesis = r; out.tier = model;
        log({ event: 'synthesis_ok', tier: model });
        break;
      }
      log({ event: 'synthesis_rung_failed_or_empty', model });
    }

    if (!out.synthesis) {
      const floor = buildFloor(findings);
      if (matchesSynthSchema(floor)) {
        out.synthesis = floor; out.tier = 'floor';
      } else {
        out.synthesis = { prioritized: [], summary: `Floor off-schema; ${findings.length} findings preserved in return.` };
        out.tier = 'floor-invalid';
      }
    }
  } catch (err) {
    log({ event: 'synthesis_threw', message: String(err && err.message) });
    if (!out.synthesis) {
      out.synthesis = { prioritized: [], summary: 'Synthesis threw; findings preserved in return.' };
      out.tier = 'floor-error';
    }
  }

  log({ event: 'synthesis_done', tier: out.tier, checkpointed: out.checkpointed, findings: findings.length });
  return out;
}

// ===========================================================================
// FINDERS
// ===========================================================================
function finderPrompt(area, round) {
  const lensText = area.lenses
    .map((l) => `- ${l}: ${LENS_GUIDE[l]}`).join('\n');
  const dedup = round > 0
    ? `\nThis is round ${round + 1} of a loop-until-dry sweep. Report ONLY NEW ` +
      `findings not an obvious restatement of an earlier one; if you find ` +
      `nothing new, return an empty findings array.`
    : '';
  return (
    `You are a meticulous C++23 reviewer auditing ONE area of the claude-bus ` +
    `broker. Area: "${area.title}".\n` +
    `READ these files in full: ${area.files}\n` +
    `Consult these design docs FIRST — they define the area's INTENDED ` +
    `invariants, so you can distinguish a real bug from deliberate design: ` +
    `${area.docs.join(', ')}\n\n` +
    `Hunt through these lenses:\n${lensText}\n\n` +
    `For each finding give a precise file:line, a falsifiable claim, why it ` +
    `matters, and a surgical suggested fix. Prefer FEWER, HIGHER-confidence ` +
    `findings over a long speculative list. A deliberate escape-hatch, a ` +
    `documented seam, or mid-build-inert code is NOT a finding — note it as ` +
    `low/refutable if unsure.${dedup}\n\n` +
    `Return strictly per schema with area="${area.key}".`
  );
}

// Run one area's find stage: a single finder, plus a loop-until-dry tail for
// heavy (audit-targeted) areas. Returns a flat findings[] with ids namespaced.
async function findArea(area) {
  const all = [];
  const maxRounds = area.heavy ? 3 : 1;
  let dry = 0;
  for (let round = 0; round < maxRounds; round++) {
    const r = await agent(finderPrompt(area, round),
      { label: `find:${area.key}${round ? ':' + round : ''}`, phase: 'Find',
        schema: FINDINGS_SCHEMA });
    const got = (r && Array.isArray(r.findings)) ? r.findings : [];
    if (got.length === 0) { if (++dry >= 1 && round > 0) break; }
    for (let i = 0; i < got.length; i++) {
      all.push({ ...got[i], area: area.key,
        id: `${area.key}-${round}-${i}` });
    }
    if (area.heavy && got.length === 0) break;
  }
  return all;
}

// ===========================================================================
// ADVERSARIAL VERIFY — 3 distinct-lens skeptics per finding; >=2 refutes kills.
// The design-intent skeptic is the explicit guard against the blind-audit
// false-positive rate (verify-before-remove for escape-hatches / inert code).
// ===========================================================================
const VERIFY_LENSES = [
  {
    key: 'correctness',
    prompt: (f) =>
      `Skeptic (correctness). A reviewer claims this claude-bus bug:\n` +
      `  ${f.file}:${f.line} [${f.severity}/${f.lens}] ${f.title}\n` +
      `  claim: ${f.claim}\n` +
      `READ the actual code at that location. Reconstruct the CONCRETE ` +
      `execution that makes the claim true. If you cannot build a concrete ` +
      `trigger, or the code is actually correct, set refuted=true. Default to ` +
      `refuted=true when uncertain.`,
  },
  {
    key: 'design-intent',
    prompt: (f) =>
      `Skeptic (design-intent). A reviewer claims this claude-bus issue:\n` +
      `  ${f.file}:${f.line} [${f.severity}/${f.lens}] ${f.title}\n` +
      `  claim: ${f.claim}\n` +
      `READ the surrounding code, its CALLERS, and the relevant design doc in ` +
      `docs/. Is this DELIBERATE — a documented seam, an intentional ` +
      `escape-hatch, mid-build-inert code, or behavior the design explicitly ` +
      `chose? If it is intended-by-design, set refuted=true and cite the doc/` +
      `comment. This guards against killing working code that merely looks ` +
      `odd. Default to refuted=true when the design plausibly intended it.`,
  },
  {
    key: 'impact',
    prompt: (f) =>
      `Skeptic (impact/repro). A reviewer claims this claude-bus issue:\n` +
      `  ${f.file}:${f.line} [${f.severity}/${f.lens}] ${f.title}\n` +
      `  claim: ${f.claim}\n` +
      `Construct the exact input/sequence that would TRIGGER it in real fleet ` +
      `operation. If it is unreachable in practice, or the impact is ` +
      `negligible (cosmetic, never-hit error path), set refuted=true. Default ` +
      `to refuted=true when impact is negligible or unreachable.`,
  },
];

async function verifyFindings(findings) {
  if (!Array.isArray(findings) || findings.length === 0) return [];
  const verified = await parallel(findings.map((f) => async () => {
    const mustCall =
      '\n\nYour FINAL action MUST be the StructuredOutput call with ' +
      '{refuted, reason}. Analysis prose alone is NOT a valid completion — ' +
      'reach a verdict and emit it even if you are uncertain (default refuted).';
    const votes = await parallel(VERIFY_LENSES.map((v) => () =>
      agent(v.prompt(f) + mustCall,
        { label: `verify:${f.id}:${v.key}`, phase: 'Verify', schema: VERDICT_SCHEMA })));
    const real = votes.filter(Boolean);
    const refutes = real.filter((x) => x.refuted).length;
    // Quorum over the verifiers that ACTUALLY ran (a schema-less subagent
    // yields null, not a verdict). Killed when a majority of real verdicts
    // refute (refutes*2 > real.length): 2-of-3, 2-of-2, or the lone 1-of-1.
    // ZERO real verdicts => unverified, NOT confirmed — the all-failed case
    // must never masquerade as survived (the bug this run exposed).
    const unverified = real.length === 0;
    const survives = !unverified && refutes * 2 <= real.length;
    return {
      ...f,
      verdicts: real.length,                   // how many verifiers actually ran
      votes: real.length - refutes,            // surviving (non-refute) votes
      refutes,
      survives,
      unverified,
      refutations: real.filter((x) => x.refuted).map((x) => x.reason),
    };
  }));
  return verified.filter(Boolean);
}

// ===========================================================================
// MAP MODE — comprehension / handoff artifacts.
// ===========================================================================
function explainPrompt(area) {
  return (
    `You are documenting ONE area of claude-bus for sulin's GUIDED 1:1 review ` +
    `— he will DIRECT a redesign, so ENABLE his judgment, do NOT prescribe a ` +
    `design.\nArea: "${area.title}".\n` +
    `READ in full: ${area.files}\n` +
    `LEVERAGE existing docs (cite them; do not re-derive): ${area.docs.join(', ')}. ` +
    `Where the doc and the code DIVERGE, flag it explicitly.\n\n` +
    `Produce, per schema:\n` +
    `- what: what this area does, concretely.\n` +
    `- why_shaped_this_way: the design pressure that forced this shape.\n` +
    `- why_valuable: what concretely REGRESSES if it were removed/simplified ` +
    `away (the load-bearing reason it earns its place).\n` +
    `- entry_points: the functions/files to read first.\n` +
    `- intra_area_reading_order: the order to read this area's files.\n` +
    `- design_tensions: where the bodies are — alternatives considered, sharp ` +
    `edges, things a redesign would have to preserve or could reconsider.\n` +
    `Return with area="${area.key}".`
  );
}

async function runMap(areas) {
  const briefs = (await parallel(areas.map((a) => () =>
    agent(explainPrompt(a), { label: `explain:${a.key}`, phase: 'Explain',
      schema: EXPLAIN_SCHEMA })))).filter(Boolean);

  // Durable: briefs ARE the return value's primary field. Synthesis assembles
  // the three narrative artifacts on top; if it fails, briefs still ride home.
  const out = { mode: 'map', briefs, artifacts: null, tier: 'pending' };
  try {
    const assembly = await tryStage(() => agent(
      `Assemble these per-area claude-bus briefs into a handoff for a guided ` +
      `1:1 codebase review. Use ONLY the briefs provided. Produce three ` +
      `artifacts as markdown strings:\n` +
      `- systemMap: the areas, the library DAG (bus_core -> bus_readers -> ` +
      `bus_policy; bus_pane; the bus binary), the data flow (events.jsonl -> ` +
      `readers -> policy -> delivery -> pane), and the kernel triad.\n` +
      `- briefs: the per-area what/why/why-VALUABLE, surfacing tensions, NOT ` +
      `prescribing.\n` +
      `- readingOrder: a guided path kernel -> readers -> policy -> delivery ` +
      `-> broker/rpc -> pane -> cli, with a one-line reason per hop.\n\n` +
      JSON.stringify(briefs),
      { schema: { type: 'object', additionalProperties: false,
          required: ['systemMap', 'briefs', 'readingOrder'],
          properties: { systemMap: { type: 'string' }, briefs: { type: 'string' },
            readingOrder: { type: 'string' } } } }));
    if (assembly) { out.artifacts = assembly; out.tier = 'assembled'; }
    else { out.tier = 'briefs-only'; }
  } catch (err) {
    log({ event: 'map_assembly_threw', message: String(err && err.message) });
    out.tier = 'briefs-only';
  }
  log({ event: 'map_done', tier: out.tier, briefs: briefs.length });
  return out;
}

// ===========================================================================
// ENTRY
// ===========================================================================
// args can arrive as a JSON STRING (the runtime may not hand it through as an
// object) — parse defensively, else `args.mode` is undefined and mode silently
// defaults to 'review'. This bit a map-mode run once: args.mode="map" never
// routed and it re-ran review. Coerce to an object before reading.
let argv = args;
if (typeof argv === 'string') {
  try { argv = JSON.parse(argv); } catch { argv = {}; }
}
if (!argv || typeof argv !== 'object') argv = {};
const mode = argv.mode === 'map' ? 'map' : 'review';
const selected = (Array.isArray(argv.areas) && argv.areas.length)
  ? AREA_MAP.filter((a) => argv.areas.includes(a.key))
  : AREA_MAP;

if (mode === 'map') {
  log('start mode=map areas=' + selected.map((a) => a.key).join(','));
  return await runMap(selected);
}

// review mode: pipeline find -> verify, no barrier (an area verifies the moment
// its finder returns). Then dedup + bucket + hardened synthesis.
log({ event: 'start', mode, areas: selected.map((a) => a.key) });
const perArea = await pipeline(
  selected,
  (area) => findArea(area),
  (findings) => verifyFindings(findings),
);

const allVerified = perArea.filter(Boolean).flat();
const confirmed = allVerified.filter((f) => f && f.survives);
// All-verifiers-failed findings: neither confirmed nor refuted. Carried in the
// return so a human can re-verify them rather than losing them (this run's gap).
const unverified = allVerified.filter((f) => f && f.unverified);
log({ event: 'verified', total: allVerified.length,
  confirmed: confirmed.length, unverified: unverified.length });

const out = await hardenedSynthesis(confirmed, {
  checkpointPath: 'review-synthesis-checkpoint.json',
  stats: { areas: selected.length, raw: allVerified.length,
    confirmed: confirmed.length, unverified: unverified.length },
});
out.unverified = unverified;  // ride the durable return channel, not lost
return out;
