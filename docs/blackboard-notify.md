# Blackboard notify — a second coordination pattern as a Policy actor

Status: **design + plan** (elodin, 2026-06-05). Autonomous max-throughput build
(sulin's directive, via auri). Design-first, then build + land + report. Owner:
elodin. Reference: [[policy-actors]] §6 (the unification),
[[work-queue-dispatch]] (M2, the first pattern).

## 0. Why a second pattern

M2 proved the Policy substrate admits a **competing-consumer** pattern
(work-queue: 1→1, with a claim). The blackboard proves it admits the *other*
fundamental coordination shape — **fan-out** (1→N broadcast, no claim). Two
patterns of genuinely different shape on the same `PolicyActor` interface is a
strong generality statement: **the substrate isn't work-queue-shaped; it's
coordination-shaped.**

## 1. The shape contrast (the design-space map)

| | work-queue (M2) | blackboard (this) |
|---|---|---|
| shape | competing-consumer 1→1 | fan-out 1→N |
| claim | **consume-on-assign** — the head is consumed only when actually assigned (else a task with no idle agent would be lost), via `consume_from` (actor intent → loop advances the `""` cursor) | **consume-on-read** — each post is processed once; the notify is best-effort, so the loop advances a `_dispatch` cursor as it folds the post into the snapshot. A missed notify is benign (the board is pull-able). |
| actor state | a cadence; the claim lives in the kernel cursor | **none — stateless.** The watermark is the loop's `_dispatch` cursor; the actor is a pure function of the snapshot |
| vocabulary | `Enqueue` + `consume_from` field | **pure `Enqueue`** — no field, no kind |

The contrast is the point: a claim pattern needs consume-on-assign (the
`consume_from` extension); a fan-out pattern needs only pure `Enqueue` and a
loop-owned read cursor. The `BlackboardActor` is the **purest actor yet** — fully
stateless.

## 2. The `BlackboardActor`

On a new post to a `blackboard` topic, notify every live agent (except the
post's author) that the board changed — delivered as ordinary inbox mail.

```
class BlackboardActor : public policy::PolicyActor {
  // name() = "blackboard"; no member state — stateless.
  // evaluate(ctx): for each ctx.board_updates entry (a new post the loop
  //   folded in), for each ctx.agents agent != the post's author, emit
  //   Enqueue{ inbox-<agent>, "blackboard '<topic>' updated by <author>:
  //   <body>", protocol = "blackboard-notify" }.  Pure Enqueue, fan-out.
};
```

### 2.1 The snapshot fold (loop plane)

`PolicyContext` gains `std::vector<BoardUpdate> board_updates` (`{topic, author,
body}`). The loop, per `blackboard` topic, peeks the head past a loop-owned
`_dispatch` consumer cursor, folds each new post in, and **advances `_dispatch`
past it** (consume-on-read — the loop owns the watermark, so the actor stays
stateless). Bounded window per tick. The `_dispatch` cursor is a persisted
cursor file, so a broker restart resumes *past* already-notified posts — no
re-notify. (A pre-existing blackboard never yet folded is notified once on first
adoption — a benign one-time burst.)

### 2.2 Guards held

- **§1.4 no-cursor-verb**: the *actor* advances no cursor; the *loop* advances
  the `_dispatch` consumer cursor (kernel plane). No new field or kind.
- **kernel triad**: the `_dispatch` cursor is a fresh consumer cursor on a
  blackboard topic — no agent-inbox at-least-once cursor is touched; the
  notify Enqueues are epoch-stamped like every record.
- **§2.1 composition**: the engine still never arbitrates; notifies are
  idempotent-ish heads-ups, so duplicate/overlap resolves harmlessly.
- **§4 leaf guard**: trivially — the actor owns *no* mutable state.

## 3. Activation (the deliverable insight, again)

```
bus topic create notes --kind blackboard
bus msg enqueue notes "decision: we ship Plan A"   # any agent posts
```

The `BlackboardActor` already ships and is registered (inert until a blackboard
topic exists). Every live agent gets a notify; they read the full board with
`bus msg peek notes` (non-destructive). `coordination/blackboard/README.md`
documents it — again **no loader, no hooks/, no agent-prompt**: a second pattern
that is broker-side code that already ships.

## 4. Build + verify

- **Code**: `blackboard_actor.{h,cpp}` (bus_policy, links bus_readers only);
  `PolicyContext.board_updates`; the loop fold (`foldBoardUpdates`, advancing
  `_dispatch`); register beside Recovery/Dispatch in `load()`; inert until a
  blackboard topic exists.
- **Tests (litmus, broker-free)**: `BlackboardActor::evaluate` — a board update +
  three agents (one the author) → two notify Enqueues (author excluded); empty
  `board_updates` → no actions.
- **Itest (isolated)**: create a blackboard topic, post once, drive ticks →
  every live fake-agent except the author gets a `blackboard-notify` in its
  inbox; a second tick does not re-notify (the `_dispatch` cursor advanced).
- **Link guard**: `nm` — `blackboard_actor.o` references no pane/socket symbols.

## 5. MVP scope (deliberate)

- **Notify all live agents** (minus author). Subscriber filtering / opt-in,
  rate-limiting noisy boards, and a "board changed, go read" heads-up vs.
  full-body broadcast are follow-ons. The MVP proves fan-out, not a pub/sub
  feature set.
- **Consume-on-read** means a notify is best-effort: an agent offline when a post
  lands won't get a retro-notify (it reads the current board on arrival). Correct
  for a shared-state blackboard.
