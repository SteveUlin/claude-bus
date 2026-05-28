# bast — ideation: what would meaningfully improve claude-bus

Auri ask. Five concrete ideas, ordered by leverage-per-effort. Effort:
XS (<1 hr), S (half-day), M (1–3 days), L (>3 days). Picks come from
patterns I hit this session, not just my layout/wiring territory.

## 1 · Commit attribution from `$CLAUDE_BUS_AGENT_ID` — XS

Every commit shows author *Steven Ulin*; the actual agent is buried
in the message body if at all. A pre-commit hook in `settings/hooks/`
that injects `Co-Authored-By: <agent-id> <noreply@claude-bus>` from
`$CLAUDE_BUS_AGENT_ID` makes attribution visible in `jj log`,
GitHub, and `git blame -L`.

Side benefit: the scoop-bundle problem I hit three times this session
(another agent's `git add -A` swept up my uncommitted edits — see
69f9128, 975fe28, possibly more) becomes diagnosable from `jj log`
alone. If `Co-Authored-By: bast` appears under a kvothe-authored
commit, the misattribution stands out.

Effort: ~10 lines of bash in a `pre-commit` hook, plus a one-line
entry in `.git/hooks/pre-commit` shim. No infra.

## 2 · Live layout drift + `bus layout patch` — M

Almost every `layouts/fleet.kdl` change ships with a "picks up on
next relaunch" caveat. Sulin then has to remember to relaunch — or
live with stale layout. Doing it live requires the focus-shift dance
(`focus-pane-id` → `new-pane` → `stack-panes` → restore) that I had
to write by hand for the jj-log addition.

Proposed:
- **`bus layout diff`** — parse `layouts/fleet.kdl`, query
  `zellij action list-panes` / `query-tab-names`, report panes
  present-in-layout-not-in-session and vice versa.
- **`bus layout patch`** — for the additive subset of diffs (new
  pane in existing tab, new stacked entry), call zellij action verbs
  to apply with focus save/restore baked in. Refuse non-additive
  diffs (renamed/moved/deleted panes).

Effort: M. Diff algorithm + the focus-save-restore wrapper.

## 3 · Worker auto-clear triggers from `docs/context-budget.md` — S

Elodin's spec (`docs/context-budget.md` §"Recommendation") proposes
shipping **trigger (2) idle + post-task** + **(3) cache-TTL gate**:
~30 lines in the broker observing `events.jsonl`, enqueueing
`/clear` on workers when `last_event == Stop`, `idle ≥ 10 min`,
`inbox-<agent>` empty, in-flight empty, cache cold (`>5 min since
last assistant turn`), and a "done" signal in the last 20 min.

This removes the "agent fills context until claude-code panics and
auto-compacts mid-task" failure mode. Doc-blocked on nothing — spec
is concrete, the broker already watches events.jsonl, and the
commands-<agent> dispatch path is built.

Effort: S–M. Most of it is the heuristic; the dispatch is one call.

## 4 · `bus health` probe + monitor-bar surfacing — S

The broker delivery wedge (see auto-memory `broker_delivery_wedge`)
was diagnosed only after sulin noticed staleness. A `bus health`
verb returning red/green per component:

- broker RPC roundtrip latency
- delivery-loop liveness (last on_tick timestamp)
- per-agent inbox-depth + oldest-record age
- presence-file freshness vs. agent's actual pane state

Surface a one-character indicator in the monitor header (or
agent-bar) so sulin sees red before queueing a message that won't
land. The probe data is already collectable via existing RPC + state
files.

Effort: S. ~80 LOC for the probe verb, ~10 LOC monitor integration.

## 5 · Hook-script shared library — XS

Every script in `settings/hooks/` reinvents the boilerplate:
`NAME=${CLAUDE_BUS_AGENT_ID:-}` early-exit, `STATE=${CLAUDE_BUS_STATE
:-/tmp/claude-bus}`, atomic-rename writes, jq null-safe field
extraction. I wrote it again today in `settings/statusline-write.sh`.

A `settings/hooks/lib.sh` with sourced helpers (`require_agent_id`,
`write_atomic`, `jq_field`, `state_dir`, `mkdir_p_state`) cuts ~10
lines per hook and prevents drift (e.g. one hook uses `$STATE/x`,
another spells it `$CLAUDE_BUS_STATE/x` — already happening).

Effort: XS for the library; small follow-up to retrofit existing
hooks (`log-event.sh`, `agent-register.sh`, `focus-write.sh`,
`statusline-write.sh`).

## Notes on cross-territory picks

(2) layout patch touches my floor (`layouts/`) but the diff algorithm
is C++ work in `src/sub/sub_layout.cpp`, more elodin/kvothe-shaped.

(3) worker auto-clear is broker-shaped — elodin's territory; my
contribution is the role-prompt update in `roles/{bast,kvothe,elodin}.md`
to opt agents in.

(4) health probe spans broker + monitor — split across elodin
(probe) and kvothe (surfacing).

If auri's synthesis converges on any of these, I can own the
settings/hooks/launcher side end-to-end. Anything broker- or
viewer-shaped should land with the right peer.
