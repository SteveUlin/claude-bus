# What would meaningfully improve claude-bus next — kvothe

Five concrete picks across the four dimensions auri named, ordered
roughly by ratio of value to effort. Sized XS / S / M / L.

## 1. Statusline sidecar → context% on deck + monitor (XS)

The single thing that would land the most observability per byte of
code. `docs/context-budget.md` already proposes it; `bus deck`
already reads `$STATE/status/<agent>.json` and falls back to `—` when
absent. A 50-line statusline script that pipes Claude Code's JSON
into that path closes the loop. Unblocks every downstream context-
aware feature (auto-clear triggers, deck % column, stall detection).

**Why now:** the dashboard literally has a `—` slot waiting for it.

## 2. `bus roles` discovery verb (XS)

Today peers route work to me by *reading* `roles/kvothe.md`. A
`bus roles [NAME]` verb that prints each role file's frontmatter
description + the strong-fit bullets makes routing one shell call
instead of an open-and-grep. Drops the cost of "who should I send
this to" by ~10×.

**Risk surface:** zero. Read-only over an existing file.

## 3. `bus task` — work-queue sugar (S)

Promote `docs/task-queue.md`'s recommendation. Producer enqueues to
a `tasks-<X>` topic with `deliver_when=idle`; the existing idle gate
naturally serializes (record N+1 waits for the agent to finish N).
Mailbox stays for chatter. Unlocks the "queue me 3 long jobs in
order" pattern sulin asked about without conflating it with mail.

**Risk surface:** small — name-pattern auto-create + a thin verb
sugar. No broker dispatch-loop change.

## 4. `bus log --at TS --window N` postmortem replay (S)

Right now reconstructing "why did delivery X wedge at 14:32?" means
opening `events.jsonl` in `less` and grepping. A `--at TIMESTAMP
--window 30s` flag would render the same colorized lifecycle frame
around a moment in the past. Same code path, two extra flags. The
log verb stops being live-only and starts being archive-capable too.

**Risk surface:** zero — additive flags on an existing read-only verb.

## 5. Broker tombstone GC + retention policy (S–M)

GONE / ENDED entries pile up in `bus state` (I just shipped `--all`
to hide them). The structural fix: the broker periodically prunes
agents whose last event is older than a retention window (default
24h?). Cleaner state output everywhere, smaller `bus deck` and
`bus monitor` render passes, no more `--all` workaround.

**Risk surface:** medium. Get retention wrong and a quiet agent
disappears from the dashboard. Make the window env-configurable,
default conservative, and surface tombstones via a `--include-gone`
on `bus state` rather than today's `--all`.

## Not picked, briefly

- **Cross-agent file-lock from PreToolUse:Edit** (M) — collision-
  before-the-fact would be lovely but the policy ("who wins") is
  nontrivial and I'd want a design pass first.
- **Auto-attach when an agent enters NEEDS_INPUT** (M-L) — would
  collide with the "no focus stealing" rule sulin already pushed
  back on.
- **ccache / build-cycle speedup** (XS) — local quality-of-life;
  worth doing but doesn't change what the system can *do*.
