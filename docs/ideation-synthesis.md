# Ideation synthesis — what to ship next

Source: `docs/{bast,kvothe,elodin}-ideation.md`, 15 ideas across the
three peers. This synthesis ranks by impact-vs-effort, calls out
overlap and convergence, and separates what's already in flight from
what's worth picking up.

## Top 5 picks

### 1 · Broker health surface (S) — two-peer consensus

- **elodin #1** proposes `broker_stats` RPC: `tick_count`,
  `last_tick_elapsed_ms`, `scan_events_bytes_consumed`,
  `retries_fired_total`, `escalations_total`, `auto_clears_total`.
- **bast #4** proposes `bus health` verb (red/green per component)
  + monitor-bar one-char indicator.
- Combined: elodin's instrumentation behind the RPC, bast's verb +
  monitor surface on top.
- Why first: the broker-delivery-wedge class of bug is the most
  expensive recent debug cost (alive + RPC-responsive, internal loop
  stalled). Every future reliability fix benefits from this being
  in place; without it each new failure mode pays the same
  heartbeat-instrument-then-yank cost.
- Owners: elodin (probe + RPC), kvothe (monitor column).

### 2 · Commit attribution from `$CLAUDE_BUS_AGENT_ID` (XS) — bast #1

- Pre-commit hook injects `Co-Authored-By: <agent-id>
  <noreply@claude-bus>` from `$CLAUDE_BUS_AGENT_ID`.
- Diagnostic value: the scoop-bundle pattern (one agent's commit
  sweeping up another's uncommitted edits) recurred ≥3× this
  session — 975fe28 and 69f9128 are confirmed instances. With
  attribution, mismatched authors under one commit message stand
  out in `jj log`.
- ~10 LOC bash. Owner: bast.

### 3 · `bus roles` discovery verb (XS) — kvothe #2

- Print each role file's frontmatter description + strong-fit
  bullets. Routing becomes one shell call instead of open-and-grep.
- Cuts the per-dispatch routing cost ~10×. Especially useful for
  auri (hub) and any future role-aware automation.
- Read-only over existing files. Zero risk. Owner: kvothe.

### 4 · Default TTLs per topic kind (XS) — elodin #3

- `ttl_ms` is in the wire format but always 0 today.
- Proposed defaults: `agent-inbox` / `tui-commands` → 1 h;
  `audit` / `append-log` → durable; `work-queue` / `pubsub` /
  `blackboard` → semantically forever.
- Prevents the post-restart `inbox-ops` escalation spam (100+
  recursive stale-epoch records) from recurring.
- Few-dozen LOC CLI + broker. Owner: elodin.

### 5 · Worker auto-clear triggers (S–M) — bast #3 + elodin shape

- Ship `docs/context-budget.md`'s **(2) idle + post-task** + **(3)
  cache-TTL gate** triggers. ~30 LOC observing `events.jsonl`,
  enqueueing `/clear` on workers when last_event=Stop, idle ≥
  10 min, inbox empty, in-flight empty, cache cold, done-signal
  recent.
- Addresses the "agent fills context until Claude Code panics
  and auto-compacts mid-task" failure mode — the one that
  silently corrupts continuity.
- Owners: elodin (broker code), bast (role-prompt opt-ins).

## Strong runners-up (XS or S; defer-not-discard)

- **kvothe #4** — `bus log --at TS --window N` postmortem replay.
  Additive flags on an existing verb. Zero risk. S.
- **bast #5** — `settings/hooks/lib.sh` shared library
  (`require_agent_id`, `write_atomic`, `jq_field`). Kills the
  boilerplate every new hook reinvents. XS.
- **elodin #4** — hard-death detection (phase 2 of #10). Closes
  the "pane vanishes without SessionEnd → records pile up forever"
  class. M, needs care.

## Already in flight (excluded from this synthesis)

- **Hybrid delivery** (elodin #2 = #47) — approved, shipping.
- **Statusline sidecar** (kvothe #1) — landed in 69f9128 today;
  ctx column on `bus deck` now populated.

## Defer / lower priority

- **bast #2** — `bus layout diff` + `bus layout patch`. M effort,
  less reliability impact than the top 5. The "next-relaunch" caveat
  is annoying but lives below the line for now.
- **kvothe #3** — `bus task` work-queue sugar. Niche unless sulin
  asks for the "queue 3 long jobs in order" pattern explicitly.
- **kvothe #5** — broker tombstone GC. `bus state --all` works
  around the pile-up today; structural fix can wait.
- **elodin #5** — rebuild integration tests. L effort, schedule as
  its own block rather than interleaved with feature work.

## Convergence

Three themes emerge across the 15 picks:

- **Observability** — kvothe #4 (log replay), bast #4 (health verb),
  elodin #1 (broker stats). Strongest theme; #1 above bundles two.
- **Reliability** — elodin #3 (TTL), elodin #4 (hard-death),
  bast #3 (auto-clear). Three failure classes addressed.
- **Dev ergonomics** — bast #1 (attribution), bast #5 (hook lib),
  kvothe #2 (roles verb). All XS; ship in a batch.

## No conflicts

The 15 picks contain no internal contradictions. Peers differ on
prioritization (bast → attribution + layout; kvothe → UX + deck
followups; elodin → reliability) — that's diversity by territory,
not disagreement.

## Recommended batch order

If sulin wants a single "ship these next" batch:

1. The three XS dev-ergo wins (#2, #3, #4 above + bast #5) — one
   afternoon. Quick visible improvement.
2. **#1 broker health surface** — most leverage; unblocks the next
   reliability fixes.
3. **#5 worker auto-clear** — closes the auto-compact failure mode.
4. **elodin #4 hard-death** — closes the last big silent failure.

Step 1 + 2 alone is a half-day of work for a notable improvement in
fleet introspection and dev experience.
