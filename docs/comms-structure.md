# comms structure — reducing per-turn latency

Trigger: in a 5-minute window, sulin sent three concurrent messages
while comms was working on the first. They piled up because comms
processes inbound mail FIFO. A Sonnet swap was reverted — sulin's
read is that *structure*, not model, is the lever. This doc
inventories where the seconds go today, lists restructurings with
tradeoffs, and recommends a sequence.

Background read: `roles/comms.md`, `docs/comms.md`, `docs/comms-routing.md`,
`docs/fast-comms-eval.md`, `feedback_comms_less_formal`, `feedback_comms_no_constant_approval`.

## (a) Per-turn cost inventory

A routine `[<sender>] message` turn:

- **Input prefix.** System + `CLAUDE.md` hierarchy + `roles/comms.md` +
  first 200 lines of `MEMORY.md` + transcript. ~3–6k tokens, cached
  (~0.1×) inside the 5-min TTL, fresh (~10×) after idle.
- **Discovery.** `bus introduce <sender>`, sometimes `dump-screen` /
  `bus state`. Each is a Bash subprocess + RPC; pane-state paths spawn
  zellij. 2–5 calls per turn → **0.5–2 s wall time**.
- **Reasoning + draft.** ~1.5–3 s on Sonnet for the role's verbose flow.
- **Approval gate.** Feedback memos already permit skipping for clear
  intent, but the role still describes the verbose floor, so the model
  often draws it out.
- **Send + recap.** ~0.5 s for `bus msg mail`, then a recap sentence
  the feedback memos already say to drop.

Per turn: **roughly 3–8 s** wall time + sulin's read/approve. Three
concurrent messages serialize and each re-pays discovery + drafting,
so pile-up grows linearly. Perceived delay is worse because sulin
sees no acknowledgement until turn 1 fully resolves. (Numbers are
estimates; worth instrumenting before any big change.)

## (b) Restructuring candidates

**(1) Skip cold `bus introduce` for warm peers.** If the sender's last
event is <5 min old, the transcript already carries the context.
Codify in the role: only `bus introduce` when the peer hasn't
surfaced in the last 5 turns or 5 min. **Saves ~0.3–1 s on ~60% of
turns.** Risk: stale context on the long tail; mitigated by sulin's
pane visibility.

**(2) Inline the routing decision table into the role.**
`docs/comms-routing.md` is on-demand today. Inline its ~40-line
decision table into `roles/comms.md`; keep the rationale external.
Watch the 200-line ceiling per Anthropic's memory doc.
**Saves ~0.2–0.5 s on `/dispatch`** and removes a wrong-peer step.

**(3) Acknowledge-then-work for pile-ups.** On a new mail mid-turn,
emit "[comms] queued: <topic>, N ahead" before processing FIFO.
**Wall time unchanged; perceived pile-up dissolves.** Cost: one extra
send per pile-up plus a depth-check at turn start.

**(4) Match the role prompt to the feedback memos.**
`feedback_comms_less_formal` + `feedback_comms_no_constant_approval`
already describe the actual behavior; the role's §"approval rule" and
§"after sending" still describe the verbose floor. Rewrite to match.
**Saves ~1–2 s per routine send** and removes the contradiction the
model reconciles every turn.

**(5) Planner peer split.** Per `fast-comms-eval.md` §7: relay on
Haiku/Sonnet, planning on Opus with a cache-pin heartbeat. Biggest
lever, biggest project. Defer until small wins are in.

**(6) Pre-draft dispatch templates.** Invites staleness; low marginal
value once (2) lands.

## (c) Recommendation

Stack the small wins before the structural one. **Do (4), (1), (3),
(2), in that order.** Each is a `roles/comms.md` edit; no new agents,
no model swap, no new infra.

1. **(4) first — closest to free.** The feedback memos already say
   what the behavior is; the role prompt just has to stop describing
   the old one. Lands today, ~30-line diff.
2. **(1) next — warm-peer discovery skip.** One paragraph in the role.
   Lands today.
3. **(3) third — ack-then-work.** One rule + a check on `bus msg peek
   inbox-comms` depth at turn start. Verify the perceived-pile-up
   effect on the next concurrent burst.
4. **(2) last of the cheap set.** Inline the routing table. Mind the
   200-line ceiling; cap the role at ~150 to leave headroom.

**Combined effect:** ~2–4 s saved per routine turn plus the
ack-then-work pattern removing the perceived pile-up even when wall
time hasn't moved. **Defer (5) and (6).** The planner-peer split
remains the right end-state; the small wins above will reveal whether
residual pile-up still justifies it. Templates are maintenance-heavy
and overlap with what (2) already encodes.

No code in this task. The first PR-shaped change is a single
`roles/comms.md` edit implementing (4) and (1) together.
