# Fresh-spawn first-brief delivery (harness-gap #4)

**Symptom (auri, taro muse):** a freshly-spawned off-TTY agent registers
(`SessionStart`) and sits idle at an empty, input-ready prompt. Its first
brief (`bus msg mail <agent> …`) never lands until a manual `bus msg send`
nudge gives it a turn. Every fresh peer spawn hits it.

## What the broker actually sees (trace)

Off-TTY is the fleet default, so the first brief is delivered by the
**doorbell** (`maybeWakeIdleOffTty`): when an idle off-TTY agent has queued
mail, the broker rings `[bus-wake]`, which fires `UserPromptSubmit`, on which
the drain hook delivers the mail as `additionalContext`. The doorbell only
rings an agent it considers *ready*:

    ready_at_prompt   = process==Alive && turn==Ready     // Stop / idle / resume / clear
    idle_after_compact= process==Compacting

A fresh spawn's last (only) event is `SessionStart` with `source=startup`.
`computeAxes` classifies that as **`Starting`** (age ≤ 30 s) then **`Stuck`**
(age > 30 s) — never `Alive`, never `Compacting`. So the doorbell **never
rings it**, and the brief strands. The manual `bus msg send` works only
because it injects a turn → `UserPromptSubmit` → `Alive`/`Working`, and the
drain on that turn delivers the brief.

The trap is fundamental to events-only classification: a fresh agent waiting
for its first prompt and a genuinely wedged boot are **identical from events
alone** — both are "`SessionStart`, no follow-up event." That indistinguish-
ability is exactly why `Stuck`/BOOT_STUCK exists.

## The disambiguator: pane mode

The pane's input mode is the ground truth events can't give:

- **Fresh agent ready for input** → claude sits at an editable prompt →
  `pane.mode == "INSERT"`.
- **Genuinely wedged boot** (startup modal / spinner / hung init) → a modal
  overlay, *not* an editable prompt → `pane.mode != "INSERT"`.

`dispatchAgentInbox` (the TTY-push path) already encodes this:
`State::Idle || (State::Starting && pane.ok && pane.mode == "INSERT")`. The
doorbell simply never adopted it — and it calls `computeAxes` with **no
pane**, so it can't see the mode at all.

## The fix (scoped to the doorbell)

A pure predicate `wakeReadyForMail(ax, pane)` in `bus_agent_status`,
unit-tested, that the doorbell calls instead of the inline check:

    Alive + Ready                          → wakeable   (unchanged)
    Compacting                             → wakeable   (unchanged, the strand fix)
    (Starting | Stuck) + pane INSERT       → wakeable   (NEW — fresh idle at prompt)
    (Starting | Stuck) + pane !INSERT/!ok  → NOT wakeable (preserves BOOT_STUCK)
    Working / NeedsInput / New / Ended / Gone → NOT wakeable

So the INSERT-rescue applies **only** to the boot-ambiguous states
(`Starting`/`Stuck`), where the pane mode resolves "ready vs wedged." Every
other state is untouched, and a wedged boot showing a modal stays `Stuck` —
**BOOT_STUCK detection is preserved exactly** (auri's explicit constraint).
`deliverInline`/`sendToPaneSafe` is the final backstop: it refuses a
scrolled/locked/modal pane, so even a misread can't type into a modal.

### Why scoped to the doorbell, not a unified readiness contract

`dispatchAgentInbox` (TTY push) and the doorbell (off-TTY) have *similar but
not identical* readiness gates today. Unifying them is tempting but widens
blast radius onto the TTY path `comms` rides. This fix touches only the
off-TTY doorbell — the path the fleet defaults to and where the gap was hit —
and reuses the same pane-INSERT signal `dispatchAgentInbox` already trusts.
A later cleanup can extract one shared predicate for both.

## Verification

- **Unit:** `wakeReadyForMail` truth table (the readiness contract).
- **End-to-end:** auri's own repro — spawn a peer, `bus msg mail` it, confirm
  the first brief lands with **no** manual nudge — is the acceptance test;
  it needs a live zellij pane reporting INSERT, which a hermetic sandbox
  can't fake, so auri re-runs the spawn flow post-land.
