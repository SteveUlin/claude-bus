# Routing heuristic for comms

How comms picks a peer for routine work without pinging sulin every time. Target: autonomous on ~80% of dispatches; escalate the rest. Source: `roles/*.md` (only `comms.md` exists today — see follow-up), the auto-memory under `~/.claude/projects/-home-sulin-claude-bus/memory/`, and recent commit topics.

This doc is meant to be **read on demand by comms before `/dispatch` or `/draft`**, not loaded into every turn. The role prompt should carry one line: "consult `docs/comms-routing.md` before dispatching." Keeps the high-frequency relay surface lean.

## Where territory currently lives

There are no per-agent role files yet (only `roles/comms.md`). Specializations are emergent from commit topics. Today's working assignment:

| Agent | Strong fit | Weak fit |
|---|---|---|
| **kvothe** | Viewers, dashboards, TUI columns (`monitor`, `agent-bar`), state-rendering, anything visible. Recent: FOCUS column, broker-down indicator, INPUT col rework. | Broker internals, wire format, deep protocol decisions. |
| **bast** | Layouts (`fleet.kdl`), `.claude/settings.json`, hooks under `settings/`, process/pane wiring. Recent: cockpit→comms restructure, comms tab shape. | Algorithmic / state-machine work inside `src/`. |
| **elodin** | Broker internals (delivery loop, retry/ack/epoch, RPC), wire-format changes, design eval docs in `docs/`. Recent: delivery-wedge fix, drop verb, epoch quarantine, clear-policy and fast-comms evals. | Pure UI/TUI rendering; pure layout edits. |

Treat these as priors, not absolutes. Verify by running `bus introduce <name>` for the candidate before drafting — that surfaces the agent's most recent activity, which is the freshest signal.

## Decision procedure

Run this in order; stop at the first rule that fires.

1. **Sulin named the peer.** Done. Skip the rest.
2. **Task is sensitive or ambiguous** (model swap, security, anything that touches another in-progress thread, naming convention proposal, anything new the agent might push back on). **Escalate to sulin** with a one-line proposed routing. Don't auto-dispatch.
3. **Cache warmth bias.** If exactly one candidate's last event is within the last 5 minutes (warm cache, see [Anthropic cache economics](../docs/clear-policy.md)) AND that candidate is a plausible fit, pick them. Cache warm = ~10× cheaper input tokens; the bias dominates ties when both are technically fit.
4. **Territory match.** Use the table above. If exactly one agent's strong-fit zone covers the task, pick them.
5. **Load distribution.** Check `bus state`. Among territory-matching candidates: prefer `IDLE` over `WORKING`; never assign to `STUCK` / `BOOT_STUCK` / `NEEDS_INPUT`. If two are IDLE, pick the one with the warmest cache.
6. **Tie-breaker.** Default to elodin for protocol work, kvothe for visible work, bast for config / wiring. If still unclear, **escalate to sulin** — that's exactly the 20% where the heuristic should give up gracefully rather than guess.

## Concrete rules

For common task shapes:

- **"Render / display / show X in the dashboard."** → kvothe.
- **"Add a column / fix a layout / change pane shape."** → kvothe if it's monitor/agent-bar code, bast if it's `layouts/fleet.kdl` or `settings/`.
- **"Add a verb / RPC / fix delivery / change wire format."** → elodin.
- **"Write a design doc / eval / proposal in `docs/`."** → elodin.
- **"Investigate why X is slow / wedged / behaving weirdly."** → match to where the bug lives (UI → kvothe; broker → elodin; layout → bast).
- **"Coordinate / triage / decide between options."** → escalate to sulin; this is judgment work, not relay.
- **"Run a test, verify a hypothesis."** → match to the relevant territory.

## What NEVER to auto-dispatch

Always escalate to sulin first:

- Anything that proposes a model change, role change, or trust-boundary change.
- Anything where two agents' recent commits suggest they're working in the same file (risk of cross-thread).
- Anything labeled urgent, blocking, or breaking, where a bad routing decision compounds.
- Anything that asks the recipient to *delegate further* — multi-hop routing is judgment work.

## After dispatching

Per `feedback_comms_less_formal.md`: skip the post-send recap unless something notable happened. The approval-gate-was-passed signal is implicit; the human can check `bus state` if they want it.

## Follow-up (out of scope for this doc)

Once this heuristic gets exercised, the right next step is **per-agent `roles/*.md` files** with declared territories — same shape as `roles/comms.md`. That removes the guesswork; routing becomes a registry lookup against `bus agents`'s `role` field (currently `(none)` for bast/kvothe/elodin). A 30-line `roles/kvothe.md`, `roles/bast.md`, `roles/elodin.md` would turn most of section 1's table into structured data and let comms route deterministically against it instead of against fuzzy priors.
