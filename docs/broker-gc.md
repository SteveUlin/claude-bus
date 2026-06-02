# Broker GC — orphan-topic reaping + double-prefix guard

Status: **design / inventory** (elodin, 2026-06-02). Read-only inventory of
the live state dir (`~/.local/state/claude-bus`) done; this note scopes the
work before any broker change. Owner: elodin.

## What the inventory changed

The queued "broker-GC" task named three parts. The inventory **reshaped it to
two** — one was already built:

| Part (as queued) | Reality |
|---|---|
| events.jsonl retention (~15M, "unbounded") | **Already solved.** D1 `trimEventsLog` caps it at 16 MB (`CLAUDE_BUS_EVENTS_MAX_BYTES`), keeps the last ~half. Landed in the retention batch (`nnnrylpnqmop`), present in the live broker's ancestry. At 11 MB it simply hasn't hit the cap — bounded, not unbounded. |
| topic-log retention | **Already solved.** D2 caps every topic log at 8 MB (`CLAUDE_BUS_TOPIC_MAX_BYTES`) + per-topic `retention_ms`, clamped to the consumer floor so undelivered/in-flight mail is never dropped. |
| orphan cursors / topics | **Real gap.** See §1. |
| `inbox-inbox-human` double-prefix | **Root-caused.** See §2. |

Net: GC is **(1) orphan-topic reaping** and **(2) a double-prefix guard +
cleanup**. Retention (the byte-volume story) is done; do not rebuild it.

## 1. Orphan-topic reaping

### Gap

`bus despawn NAME` (`sub_lifecycle.cpp:321`) prunes the dynamic-peer registry
file (`$STATE/dynamic-peers/NAME`) and closes the tab. It **never touches the
broker's topic state** — `topics.json`, `cursors/<topic>/`, `topics/<topic>.log`.
There is **no topic-delete primitive anywhere** (no RPC, no CLI). So every
despawned peer leaves `inbox-NAME` + `commands-NAME` (plus their cursors and
logs) orphaned in the broker forever.

Live orphans (agents not in the fleet, not in `dynamic-peers/`):
`inbox-{fela,mola,sim,kilvin}`, `commands-{mola,sim}` — cursor dirs + log
files + `topics.json` entries, all stale since May 31.

The D2 retention sweep trims orphan log **content** (8 MB cap) but never
**deletes** a dead topic — registry entries and cursor dirs persist regardless
of size. Volume is bounded; cardinality is not.

### Design

A **broker-owned** reap (the broker owns its `$STATE`; never an external `rm`
under the live daemon). One primitive, two triggers.

**`reapTopic(name)` — broker-side, under the broker lock.** Atomically:
- remove the `topics.json` entry,
- remove `cursors/<name>/`,
- remove `topics/<name>.log`.

**Safety gates (refuse to reap if any holds):**
- **Live agent.** `name` derives to an agent that is a fleet agent or present
  in `dynamic-peers/`. An agent's inbox is not garbage just because the agent
  is transiently absent.
- **Undrained.** Cursor `<` log EOF (records the consumer never read) or an
  in-flight entry references the topic. Orphan ≠ unread-mail-pending — dropping
  undelivered mail is data loss. Skip + report; never silently discard.

A topic that passes both gates is provably dead: no live owner, nothing
unread. Reaping it loses nothing.

**Triggers:**
- **Targeted (despawn):** `bus despawn NAME` calls `reapTopic("inbox-NAME")`
  and `reapTopic("commands-NAME")` after pruning the peer file. This closes the
  gap going forward — a despawned peer leaves no broker residue.
- **Sweep (one-shot, explicit):** `bus broker gc` (or a `gc` RPC) walks
  `topics.json`, applies the gates against the live agent set, reaps what
  qualifies, prints what it reaped and what it skipped (and why). This clears
  the 6 already-orphaned topics. **Explicit, not periodic** — a background
  auto-reaper risks racing a slow-to-register agent; the human/operator runs
  the sweep. (`log()` what was skipped so a gated-but-stale topic is visible,
  not silently retained.)

## 2. `inbox-inbox-human` double-prefix

### Root cause (evidenced)

`inbox-inbox-human` carries `kind_config.agent: "inbox-human"` (vs the real
`inbox-human` → agent `human`). Its log holds **`[elodin overnight] PIECE 1/2
DONE`** records — my own overnight status. The origin: I ran
`bus msg mail inbox-human "..."`, passing the *topic name* `inbox-human` where
an *agent name* was expected. `subMail` (`sub_produce.cpp:142`) does
`"inbox-" + agent` → `inbox-inbox-human`; `getOrAutoCreate`
(`topic_registry.cpp:166`) sees a name starting with `inbox-`, derives
`agent = substr(6) = "inbox-human"`, and **creates it with no validation**.
bast used `bus msg mail human` and hit the real topic.

This is a **missing boundary guard**, not an internal code bug. The footgun is
real and easy to hit — an agent reasons "the human's inbox is `inbox-human`,
so I mail `inbox-human`" and silently creates garbage. `getOrAutoCreate` will
auto-create *any* `inbox-*` / `commands-*` name, including ones whose derived
agent is itself a topic name.

### Fix — structural guard

In `getOrAutoCreate`, after deriving the agent (`substr(6)` / `substr(9)`),
**reject a derived agent that itself starts with `inbox-` or `commands-`**:

```cpp
const auto agent = std::string{name.substr(6)};   // inbox- case
if (agent.starts_with("inbox-") || agent.starts_with("commands-")) {
  return std::unexpected{Error{
      "refusing to auto-create nested-prefix topic \"" + std::string{name} +
      "\" (agent \"" + agent + "\" is itself a topic name); "
      "did you mean a bare agent name?"}};
}
```

This closes the hole at the only place malformed topics are born — the bad
call now fails loudly instead of silently creating a phantom inbox. Optional
nicety: a matching pre-check in `subMail`/`subEnqueue` with a "did you mean
`bus msg mail human`?" hint, but layer-1 alone is sufficient and surgical.

(Not changing `isValidTopicName` — it's a pure charset check shared by the
explicit `bus topic create` path, where a deliberately odd name is the
operator's call. The guard is specific to *auto*-derivation.)

### Cleanup

`inbox-inbox-human`'s log is already-delivered overnight status (cursor at
EOF), so it passes the §1 gates — reap it with the same `reapTopic` primitive
once that lands.

## Sequencing & deploy

1. Implement the §2 guard + the §1 `reapTopic` primitive, `bus despawn`
   wiring, and the `bus broker gc` sweep.
2. Regression (bast-style, isolated `$STATE`): (a) `getOrAutoCreate` /
   `mail inbox-human` rejects the double-prefix; (b) despawn reaps a peer's
   topics; (c) the sweep reaps the 6 live orphans and **skips** a topic with
   an undelivered record (proves the safety gate).
3. **Batch the redeploy** with the already-landed dup-fix (`9914d064`, landed
   not-live) in **one graceful broker restart** — the live broker has the
   SIGTERM shutdown-fix, so no SIGKILL is needed.

## Out of scope

- Retention / byte-volume trimming — already built (D1/D2, see
  `log-retention.md`).
- tui-commands slash re-dispatch dup — separate follow-up
  (`dup-delivery-fix.md` §"Secondary").
- OTel collector-topology REC — separate deliverable, carried alongside this
  pass.
