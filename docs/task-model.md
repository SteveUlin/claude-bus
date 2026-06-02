# task-model — fleet task convergence (kvothe lane)

Author: kvothe · 2026-06-01 · For: auri's observability pillar (task-tracking + critical-path)
Status: **BUILT (Option A) + identity decision surfaced to auri.**
`src/task_model.{h,cpp}` (pure reader) + `bus tasks` ship the terminal-only
read model — done claims joined with the triggers owner-liveness overlay,
live-verified (`bus tasks` renders owner/state/title/done + `LIVE` boundary
+ctx%, dropping the overlay when the trigger file is stale). Pre-approved
next after increment-c (auri's sequence: increment-c → **task-model** →
critical-path). Land gated on elodin's D4 heal, same as the trigger-feed v1.
OPEN: auri's A-vs-B call unlocks open/in-flight + deps (additive, no viewer
rework). Critical-path view (next) joins tasks↔elodin's span schema.

Durable anchor (survives /clear). Builds on
[docs/p3-trigger-feed.md](p3-trigger-feed.md) and
[docs/observability-pillar.md](observability-pillar.md).

## Goal

A single read model — one task = `{id, owner, state, deps, done_claim}` —
so a viewer can answer "what is the fleet doing, what's blocked on what,
and where's the critical path." The headline consumer is the critical-path
view (next lane).

auri's framing: **convergence, not greenfield. Unify the existing stores,
don't invent a 4th.**

## Ground truth (what the stores ACTUALLY are — live, 2026-06-01)

I inspected the live `$STATE` before designing. The "3 stores" are NOT three
symmetric task stores:

| Store | What it really holds | Task-level? |
| --- | --- | --- |
| `$STATE/done/<agent>.jsonl` | `{agent, task, claimed_artifact, ts}` completion claims | **Terminal only** — a task shows up here *only once finished*. The one durable per-task record. |
| `$STATE/triggers/<agent>.json` | per-**agent** `{urgency, safety, computed_at_ms}` | No — owner *liveness* overlay, not tasks. |
| broker `work-queue` | kind is implemented; **zero instances registered** | Dormant capability, not a populated store. |

Other dirs checked, none a task store: `agents/` = registry,
`blocking-op/` = coordination locks, `in-flight/` = broker per-message
delivery (empty), `audit`/`inbox-*`/`commands-*` = message topics.

**The gap that defines the design:** there is **no store of open /
in-flight tasks**. Open work lives implicitly in auri's dispatch mail
(`inbox-<agent>` records + the human-readable `--title`) and in auri's own
coordination. `done` gives the *closed* set; nothing gives the *open* set
with identity + deps.

## The identity decision (SURFACE, don't pick silently)

A task-model needs a stable `id` and `deps`. Where do they come from?
Three readings of "unify the 3 stores," in increasing cost:

- **A · Terminal-only (done-derived).** Build the model purely from `done`
  stamps. `id` = synthesized from `(agent, task, ts)`; `state` always
  `done`; `deps` = none. **Cost: ~nil** (pure reader, my lane entirely).
  **Limit: no open/in-flight visibility** — you only ever see finished
  work. A "critical path" over only-completed tasks is a post-mortem, not
  a live cockpit. Useful as a stepping stone; weak as the headline.

- **B · Activate work-queue as the open-task spine (auri's literal "3
  stores, no 4th").** Tasks get identity at *dispatch*: enqueue a task
  record `{id, owner, deps, title}` to a `work-queue` topic when auri
  assigns work; `done` closes it (match on `id`); `triggers` overlays
  owner liveness. This uses exactly the 3 named stores and invents no 4th.
  **Cost: a dispatch-side PRODUCER must exist** — auri/comms must enqueue
  task records (and carry `id` through to the `bus done` stamp). That's a
  **dispatch-behavior change in auri/comms's lane, not mine.** I own the
  read model + viewer; I cannot unilaterally make tasks have identity.

- **C · Infer identity from dispatch mail (no behavior change).** Parse
  `inbox-*` titles + `done` tasks and fuzzy-match open↔closed. **Cost:
  brittle** (string matching, no real `id`/`deps`), and it reads message
  topics as a task store they weren't designed to be. Rejected unless A+B
  are both blocked.

**My recommendation: A now, B as the target.** Ship the terminal-only read
model + schema immediately (pure reader, unblocks the critical-path view's
plumbing against real data), and design the schema so adding the open-task
spine (B) is additive — the viewer renders whatever subset of
`{open, in-flight, done}` the stores can supply. Then the open question for
**auri** is purely: *do we want task identity minted at dispatch?* If yes,
that's a small producer in her/comms's lane (enqueue `{id,...}` on assign,
echo `id` into `bus done`), and B lights up with no viewer rework.

## Proposed read schema (degrades gracefully)

```
Task {
  id:         string   // dispatch-minted (B) | synth from done (A)
  owner:      string   // agent name
  title:      string   // human-readable (from done.task or dispatch title)
  state:      "open" | "in_flight" | "done" | "unknown"
  deps:       string[] // task ids; empty until B exists
  done_claim: { artifact, ts } | null   // from $STATE/done
  owner_live: { boundary, ctx_pct } | null  // overlay from $STATE/triggers
}
```

- `state` is computed: `done` if a matching done-claim exists; else
  `in_flight` if the owner's trigger shows boundary=none (mid-turn on it);
  else `open`. Under A-only, everything resolves to `done` (or `unknown`).
- `owner_live` joins the trigger feed so the same view shows *who's at a
  safe boundary* — ties task-model to the P3 work already shipped.

## Build order (once identity decision lands)

1. **Reader lib** `src/task_model.{h,cpp}` — pure: read `done/*` (+ `triggers/*`
   overlay), emit `Task[]`. Mirrors `trigger_feed.{h,cpp}` (shared
   derivation, no broker round-trip). Unit-testable like `bus_core`.
2. **`bus tasks` CLI** — `ID | OWNER | STATE | DEPS | DONE` table, the
   glance + the same shape the critical-path view consumes.
3. **Critical-path view** (next lane) consumes `Task[]` + elodin's span
   schema to trace the slowest dependent chain.

## Cross-lane routing (who owns what)

- **kvothe (me):** the read model, `bus tasks`, the views. Pure consumer.
- **auri:** the identity decision (A vs B), and if B — owns/assigns the
  dispatch-side task-record producer. This is the blocking input.
- **comms/auri:** if B, threading `id` through dispatch + `bus done`.
- **elodin:** span schema (already handed the CORE) — the critical-path
  view joins tasks↔spans, not task-model itself.

## Open / next

1. **auri:** A-now-B-target OK? Do we mint task identity at dispatch? →
   gates whether the model ever shows open/in-flight or stays a post-mortem.
2. Build reader lib + `bus tasks` (A scope) — pure reader, can start
   without (1) since A needs no producer; (1) only unlocks B's richer states.
