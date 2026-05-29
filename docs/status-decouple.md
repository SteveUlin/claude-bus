# Decoupling `$STATE/status/<agent>.json` from the statusline

Author: elodin · 2026-05-28 · For: comms / sulin
Status: proposal — awaiting ack before implementation.

Follow-on to `observability-research.md` (which landed Design A: the
statusline script doing double duty). sulin wants to undo the double
duty: *"idk why we need the statusline to provide information for the
claude bus, that seems really inefficient."* They want their global
`~/.claude/statusline-command.sh` back as the visible statusline, and
the data-write moved off the display path.

This doc traces what the move actually costs, flags one wrinkle the
proposal didn't account for (the **denominator**), and recommends a
concrete shape.

---

## 1. What the data-write actually feeds

`settings/statusline-write.sh` projects ~10 fields into
`$STATE/status/<agent>.json`. But the *reader* contract is tiny. The
only consumers are `bus deck` and `bus monitor`'s CTX% column, and
both read exactly two fields via `scanIntAfter`:

- `context_window.used_percentage`  → the `pct` cell
- `context_window.context_window_size` → the `size` cell (display only,
  e.g. `85%/1M`)

Nothing reads `cost`, `effort`, `rate_limits`, `model`, `cwd`, or
`exceeds_200k_tokens` from the status file today. (Verified:
`grep -rn` across `src/` — only `sub_deck.cpp` and `sub_monitor.cpp`
touch `status/`, and only for those two fields.)

**So the entire job of the data-write, as wired, is to produce one
percentage and one window size.** Everything else the script projects
is dead payload.

## 2. The numerator is easy; the denominator is the catch

sulin's proposed fix — tail the per-agent transcript JSONL, read
`message.usage`, compute context % — is correct for the **numerator**.
Verified against live data:

```
context_tokens = usage.input_tokens
               + usage.cache_creation_input_tokens
               + usage.cache_read_input_tokens   # output excluded
```

The statusline's `context_window.total_input_tokens` (79267) equals
exactly `2 + 1108 + 78157` from that turn's `current_usage`. The
transcript's last `type:"assistant"` line carries the same three
fields. The watcher reproduces the numerator precisely, ≤1 turn stale
— matching the proposal's accepted tradeoff.

**The denominator is not in the transcript.** `context_window_size`
(200k vs 1M) appears nowhere in `message.usage`, nor in any transcript
record type. I checked exhaustively:

- Transcript `message.usage` / `message.model` — model string is
  `claude-opus-4-8`, no `[1m]` marker, no size field.
- No `context_window_size` / `window_size` key in any transcript across
  comms / kvothe / bast / auri sessions.
- `events.jsonl` SessionStart — the `[1m]` marker shows up on
  `source=startup|resume` model fields, but is **absent on
  `source=clear|compact`** (those payloads carry no model at all) and
  goes stale the moment the user `/model`-switches mid-session
  (elodin's last SessionStart said `claude-opus-4-7` while its real
  model was `claude-opus-4-8`).
- `bin/agent-launch` — no `--model` / window flag; the model+1m is
  chosen interactively, not pinned at launch.
- `~/.claude.json` / project `settings.json` — `model: null`, no
  per-project window. (The `[1m]` substrings in `~/.claude.json` are
  ANSI bold codes `ESC[1m`, not the 1M marker — false positive.)

**Conclusion: the window size is exposed *only* via the statusline
stdin.** This is the same con `observability-research.md` §Design B
already flagged ("doesn't give us `used_percentage` or
`context_window_size` without duplicating Claude Code's sizing logic")
— now load-bearing, because removing the statusline write removes the
one source of the denominator.

## 3. Options for the denominator

| | Source of window size | Failure mode |
|---|---|---|
| **A** | Assume per-model base (200k), escalate to 1M when observed tokens exceed it | A 1M agent under 200k tokens reads % against 200k → **overstates** CTX% (false "near-full"). For an all-1M fleet this is the *common* case → cries wolf constantly. |
| **B** | Single fleet-wide config knob `CLAUDE_BUS_CTX_WINDOW` (default 200k), + auto-escalation never-shrinks if tokens exceed it | Exact when the knob matches the fleet. If misconfigured low, escalation self-corrects past 200k. If misconfigured high (1M set, agent really 200k), **understates** → can mask a genuinely-full agent. |
| **C** | Keep a minimal hook that writes *only* the window size to `$STATE/window/<agent>` | Re-introduces a (tiny) statusline-path write — partially defeats the decouple. But window size changes rarely. |

The window is effectively a **deploy-time constant for a uniformly
launched fleet** — every agent comes up through the same path with the
same model/window. That argues for **B**: one knob, not per-agent
inference.

## 4. Recommendation

**Transcript watcher for the numerator + one config knob for the
denominator (option B), with auto-escalation as a safety net.**

- New broker-spawned watcher (or a tick inside the existing delivery
  loop — see §5) tails each live agent's transcript. Resolve the path
  straight from `events.jsonl` (latest event for the agent carries
  `payload.transcript_path`) — no cwd-encoder reconstruction needed.
- Per turn: read the last `type:"assistant"` line, compute
  `context_tokens` (formula in §2).
- `window = max(CLAUDE_BUS_CTX_WINDOW, observed_ceiling)` where
  `observed_ceiling` sticks to the largest token count seen this
  session rounded up to the next standard tier (200k → 1M). Default
  `CLAUDE_BUS_CTX_WINDOW=200000`; **the fleet layout sets it to
  1000000** so all current agents are exact.
- Write the same `$STATE/status/<agent>.json` shape, but only the two
  fields anyone reads (`context_window.{used_percentage,
  context_window_size}`) plus `agent` + `ts` for staleness. Drop the
  dead payload.
- Delete the data-write half of `statusline-write.sh`.

**sulin's follow-up (flag in the commit):** point
`.claude/settings.json`'s `statusLine.command` at
`/home/sulin/.claude/statusline-command.sh` (or delete the block so
the global one wins). The 5h/7d rate-limit row is statusline-only and
the monitor never showed it — no loss; sulin's global script renders
it for the per-agent statusline.

## 5. One open question for comms/sulin

**Watcher placement.** Two shapes:

1. **Inside the delivery loop** — add a `maybeScanTokens()` tick (like
   `maybeAutoClear()`), every ~5 s read each live agent's transcript
   tail. Pros: no new process, no new lifetime to manage, reuses the
   broker's already-running loop. Cons: adds file I/O to the broker's
   hot path (mitigated by mtime-gating — skip unchanged transcripts).

2. **A separate broker-spawned watcher** — its own process under the
   broker's lifetime (`PR_SET_PDEATHSIG`). Pros: isolates the I/O.
   Cons: a second lifetime to get right, and the broker-lifetime-fix
   pain is fresh.

I lean **(1)**: the data is low-frequency, mtime-gating makes the
common case a single `stat`, and it keeps the broker the single owner
of `$STATE` derivation. (2) only earns its keep if transcript scanning
turns out to stall the loop, which mtime-gating should prevent.

Decision needed before I implement: **(1) vs (2)**, and confirm
**option B + default knob** for the denominator is acceptable (vs
option A's no-config-but-cries-wolf, or C's keep-a-tiny-hook).
