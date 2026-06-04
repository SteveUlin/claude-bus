# Text Injection & Delivery Transport — A Deep Reference for claude-bus

> **FROZEN — pre-refactor archaeology (Phase-4 doc cleanup, 2026-06-03).**
> Documents the pre-shatter `delivery::Loop` architecture. Not maintained
> through the broker-seam refactor (`docs/broker-seam-redesign.md`); a lean set
> mapped to Log / Router / Transport / Readers is regenerated after Phase 2
> lands. Historical context, not current truth.

> Written 2026-05-28. The deep companion to `docs/modern-agent-techniques.md`,
> focused on one question: **how do you reliably get a message into a live
> Claude Code agent?** Every section leads with the *principle* — the physical
> mechanism, the race, the design pressure — then shows how the best
> implementations actually do it (with quoted code), then maps it back to our
> broker. Skim the tables; read the synthesis at the end.

claude-bus today gets text into an agent by **typing it into a shared TTY**:
`delivery.cpp::deliverInline` takes a per-pane `flock`, then
`pane.cpp::sendToPaneSafe` reads the pane's screen with `dump-screen`, normalizes
the line editor (`i` + `Ctrl-U`), and issues `zellij action write-chars` +
`send-keys Enter`. That single physical act — racing the program's reads and the
human's keystrokes through one slave fd — is the source of *every* delivery bug
in our history: mid-stream dropped turns, `detectMode` brittleness, `mode=unknown`
false defers, the slow-zellij wedge, focus contention.

The thesis of this doc, sharper than the survey's: **nobody who has built this
seriously delivers through the TTY. Not amux's watchdog, not Anthropic's own
Agent Teams.** Agent Teams uses a *filesystem inbox the agent polls* and treats
the tmux/iTerm pane as a pure **display surface** — which is precisely the hybrid
we should converge on. There are two strictly-better channels sitting unused in
our own stack: zellij's **`paste` action** (bracketed-paste framed, "faster and
more robust than write-chars," handles multi-line correctly) as a drop-in
hardening of the TTY path, and the **agent-drained control channel** (FIFO or
inbox file consumed via a `UserPromptSubmit`/`Monitor` hook) as the structural
fix that keeps the pane for the human while removing the broker→TTY write
entirely.

---

## 1. The core principle: a TUI owns its terminal, and you are a trespasser

A line-oriented interactive program like Claude Code **owns** its slave PTY. It
controls the cursor position, the input line editor, the scroll region, bracketed-
paste mode, and any modal overlay. When an outside process `write()`s bytes to
that PTY, three independent races open at once:

1. **Reader race.** The kernel PTY guarantees byte *ordering*, but nothing about
   *interleaving semantics*. Your bytes land in the input stream wherever the
   program's next `read()` happens to consume them. `pty(7)` explicitly warns
   "there may be a small processing delay between a write to the master and the
   effect being visible at the slave." Your `Enter` can land mid-paste; your text
   can land while a permission modal is up.
2. **Keyboard race.** In a *shared* session — the entire point of claude-bus —
   the human may be typing into the same pane. Two writers, one line buffer.
   Our `flock` serializes the *broker's own* writers but cannot serialize against
   the human's keyboard, which goes straight through zellij to the PTY.
3. **Mode race.** A line editor is a state machine (INSERT vs NORMAL, normal vs
   bracketed-paste, normal vs modal). To inject correctly you must *know* its
   state, which you can only *infer* by screen-scraping. `pane.cpp` is 646 lines,
   most of it reverse-engineering that state from ANSI dumps. Every claude-TUI
   layout change risks invalidating the heuristics — `detectMode`'s "bypass
   permissions" fallback (lines 354-366) is a scar from exactly that, the
   2026-05-28 bast wedge.

The honest framing, unchanged from the survey: **the flock'd TTY write is
reverse-engineering a private input protocol over a shared bus.** Every fix
narrows a failure mode without dismantling the mechanism that produces them.

### The dead end you should know about: TIOCSTI

The "clean" way to inject input into someone else's terminal *used* to be the
`TIOCSTI` ioctl — push a byte into a tty's input queue as if typed. It is **dead**:
a local privilege-escalation vector (a sandboxed process could stuff commands into
the parent shell), so Linux gates it behind `dev.tty.legacy_tiocsti` and ships it
**disabled by default since 6.2** (2023). Do not design around it. This matters
because it explains *why* everyone fell back to send-keys/write-chars: the kernel
took away the only "real" injection primitive.

---

## 2. The design space — six transports, ranked by where the race lives

| Transport | Frames input? | Races line editor? | Races human? | Human-attachable? | ACK quality | Multi-line safe? |
|---|---|---|---|---|---|---|
| `write-chars` + `send-keys Enter` (**today**) | No (char stream) | **Yes** | **Yes** | Yes | inferred (hook) | **No** (line-split risk) |
| tmux `send-keys "$msg"; sleep; Enter` | No | **Yes (worse)** | Yes | Yes | none | No |
| zellij `paste` (bracketed) + `send-keys Enter` | **Yes** (`ESC[200~…ESC[201~`) | Less | Yes | Yes | inferred (hook) | **Yes** |
| `TIOCSTI` ioctl | N/A | — | — | — | — | — (**dead**) |
| Agent-drained **FIFO / inbox file** via hook | **Yes** (out of band) | **No** | **No** | **Yes** | `UserPromptSubmit` | **Yes** |
| Headless `claude -p --input-format stream-json` | **Yes** (NDJSON) | **No** | **No** | **No** (headless) | `result` msg | **Yes** |

The pattern is stark: **the further down you move, the more races you eliminate —
until the last row, where you also lose the pane.** The two rows that keep
human-attachability *and* kill the line-editor/keyboard races are zellij `paste`
(incremental hardening) and the agent-drained channel (structural fix). Those are
the two recommendations.

---

## 3. How the best implementations actually do it

### 3.1 Anthropic Agent Teams — the authoritative answer: inbox files, pane = display

Anthropic shipped Agent Teams (experimental, Opus 4.6, 2026-02-05;
`CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1`). It is the closest official analog to
claude-bus, and its transport choice is the single most load-bearing data point
in this doc.

**Transport is a filesystem mailbox the agent consumes via a `SendMessage` tool —
not TTY injection.** Teammates message each other by name; a teammate that
finishes auto-notifies the lead. The docs are explicit: *"when teammates send
messages, they're delivered automatically to recipients. The lead doesn't need to
poll for updates."* State lives at `~/.claude/teams/{team-name}/config.json` and
`~/.claude/tasks/{team-name}/`; the team config holds runtime state "such as
session IDs and tmux pane IDs."

**The tmux / iTerm pane is a *display mode*, not a transport.** Two modes:
*in-process* (teammates inside one terminal, Shift+Down to cycle) and *split
panes* ("each teammate gets its own pane… click into a pane to interact directly.
Requires tmux, or iTerm2"). The pane exists so a *human* can read scrollback and
grab the keyboard — exactly claude-bus's differentiator. Messages do **not** flow
through `send-keys`.

The decisive confirmation is a *bug*:
[claude-code#58762](https://github.com/anthropics/claude-code/issues/58762).
When a teammate registers `backendType: "tmux"` with a pane id but is actually
spawned as a native process, messages are "routed to tmux mailbox → never
delivered." The diagnosis nails the architecture:

> "**The transport is NOT TTY send-keys** (no `tmux send-keys` call happens). The
> architecture uses **filesystem-based message inbox** that agents poll,
> regardless of backend type — but the polling location is misaligned between
> config and execution."

**Lesson for us.** The team that ships the reference TUI multi-agent product
delivers off-TTY and uses the pane only for human eyes. That is the hybrid the
survey recommended and the shape this doc argues claude-bus should adopt. Their
quality-gate hooks (`TeammateIdle`, `TaskCreated`, `TaskCompleted`, exit-2 to send
feedback and keep working) are also a direct analog of our blocking-op gate.

### 3.2 amux (mixpeek) — TTY send-keys + screen-scrape, with a self-healing watchdog

amux is the honest *opposite* example: it deliberately instruments nothing.
*"Parses ANSI-stripped tmux output — no hooks, no patches, no modifications to
Claude Code."* Delivery is `amux send <name> <text>` → tmux `send-keys`. Status
("is it done?") comes from scraping ANSI-stripped scrollback, not a structured
ACK — the same class of fragility as our `dump-screen` parsing.

What amux gets *right* is the **self-healing watchdog**, and its recovery actions
are themselves *deliveries*:

- Context low (< 50%) → send `/compact` (5-min cooldown).
- Detect `"redacted_thinking … cannot be modified"` → **restart and replay the
  last message**.
- `CC_AUTO_CONTINUE=1` → auto-respond to stuck prompts by prompt type.

The **replay-last-message-on-restart** pattern is the amux idea most worth
stealing (see §6.4): it presumes delivery can fail and bakes idempotent
redelivery into recovery. But amux confirms the negative lesson too — building on
send-keys + scrape means living with exactly the brittleness we already have.

### 3.3 Tmux-Orchestrator — the `sleep 0.5` antipattern, named

The canonical send-keys orchestrator splits message from Enter and pads with a
fixed sleep:

```sh
tmux send-keys -t "$WINDOW" "$MESSAGE"
sleep 0.5      # "Wait 0.5 seconds for UI to register"
tmux send-keys -t "$WINDOW" Enter
```

This is a *timing guess* standing in for a *readiness check*. It is strictly worse
than what we already do — our `dispatch.cpp::isReady` actually reads pane state
(`mode == "INSERT" && buffer == "(empty)"`) before sending, and retries with
exponential backoff (250 ms / 1 s / 4 s). Document Tmux-Orchestrator only as the
floor: any time-based sleep between text and Enter is a latent race; a readiness
read is the minimum bar, and we already clear it.

### 3.4 Headless `claude -p --input-format stream-json` — the race-free channel that costs the pane

Claude Code exposes a first-class programmatic input channel that never touches a
TTY: `claude -p --input-format stream-json --output-format stream-json`. Both
sides speak newline-delimited JSON. The community-reverse-engineered wire format
(undocumented beyond the flag table —
[claude-code#24594](https://github.com/anthropics/claude-code/issues/24594);
protocol docs in
[Roasbeef/claude-agent-sdk-go](https://github.com/Roasbeef/claude-agent-sdk-go/blob/main/docs/cli-protocol.md)):

- **User message on stdin** (one JSON object per line):
  ```json
  {"type":"user","message":{"role":"user","content":[{"type":"text","text":"your prompt"}]}}
  ```
- **The process stays alive across turns**: leave stdin open and write more
  user-message lines; each triggers a new turn in the same session. This is
  "streaming input mode" — the SDK's persistent-process path. (Single-shot
  `-p "prompt"` exits after one turn.)
- **Completion is a structured fact, not a guess**: a terminal
  `{"type":"result", ...}` line carries `subtype`, `is_error`, `session_id`,
  `total_cost_usd`, and token counts. That *is* your ACK — no hook inference.
- **Control plane**: `type:"control_request"` / `control_response` with a unique
  `request_id` multiplexes interrupts and permission decisions over the same pipe.
  *"Request IDs must be unique within a session."*
- `--include-partial-messages` surfaces incremental `stream`/`delta` events for
  live rendering; `--replay-user-messages` echoes your injected user turns back on
  stdout so a supervisor can confirm exactly what the model saw.

This is structured, framed, race-free, and *self-ACKing*. **Its cost is total:**
a headless `claude -p` has no interactive TUI, so there is nothing for the human to
attach to. Adopting it wholesale would turn claude-bus into amux-without-the-pane
— it would delete the moat. The right use is **bounded and headless-by-nature
work**: cron/reflection jobs, judge passes, batch fan-out — never the durable,
attachable fleet agents. (The survey's "don't rewrite on the SDK" stands; this is
the mechanism-level reason.)

### 3.5 The Agent SDK in-process channel (Python/TS)

The SDK is a thin wrapper over §3.4's stdin/stdout JSON-lines stream. Sessions are
captured from the `init` system message's `session_id` and resumed with
`resume=session_id` / `--resume`. For claude-bus this matters only as evidence
that the JSON channel is the *blessed* programmatic surface; we would shell the CLI
directly, not adopt the SDK runtime.

---

## 4. The bracketed-paste hardening you can ship this week

**Principle.** A line editor distinguishes *typed* input from *pasted* input using
**bracketed-paste mode**: the terminal wraps a paste in `ESC[200~ … ESC[201~`, and
the application treats everything between the markers as literal content — newlines
become *inserted newlines*, not *submits*. When you send a multi-line message as a
raw character stream (our `write-chars`), each embedded `\n` is interpreted as a
*line submit*, so a 3-line message can fire as 3 separate prompts (the
multi-line-paste line-splitting class of bug). Bracketed paste closes that gap and
also signals "this is one atomic chunk," which TUIs use to suppress per-keystroke
autocomplete/re-render churn — hence "faster."

**The finding:** zellij already ships this and we are not using it. From
`zellij-utils/src/cli.rs`, all four input actions take `--pane-id`:

```rust
/// Write characters to the terminal.
WriteChars { chars: String, #[clap(short, long)] pane_id: Option<String> },
/// Paste text to the terminal (using bracketed paste mode).
Paste      { chars: String, #[clap(short, long)] pane_id: Option<String> },
/// Send one or more keys to the terminal ...
SendKeys   { keys: Vec<String>, #[clap(short, long)] pane_id: Option<String> },
```

The implementation (`zellij-client/src/input_handler.rs`) confirms it "sends start
marker, then text, then end marker" using `ESC[200~`/`ESC[201~` before the bytes
reach the PTY, in Normal/Locked input modes. The zellij docs call `paste` "faster
and more robust than write-chars" and note it "handles multi-line input
correctly." This is the exact failure class the survey cited
([openclaw#18809](https://github.com/openclaw/openclaw/issues/18809)).

**What changes in our code.** `pane.cpp::sendToPane` (lines 514-528) currently does
`write-chars` then `send-keys Enter`. Swap the body to `paste` + `send-keys Enter`:

```cpp
// before: write-chars NAMED-PANE TEXT ; send-keys Enter
// after:  paste      NAMED-PANE TEXT ; send-keys Enter
runSilent({"zellij","action","paste","--pane-id",pane_s.c_str(),text_s.c_str()});
runSilent({"zellij","action","send-keys","--pane-id",pane_s.c_str(),"Enter"});
```

Caveats to verify on the prototype: (a) Claude's TUI must have bracketed-paste
*enabled* (it does — it's a Shift+Enter-aware editor); (b) `paste` routes through
markers only in **Normal/Locked** input mode per the source — confirm it behaves
when the pane is at the prompt; (c) the draft-restore path (`sendToPaneSafe` lines
602-606) should also use `paste` for consistency. This is a ~4-line change that
removes the multi-line-split risk and likely shaves the per-write latency. **Low
effort, medium-high payoff, fully reversible.**

---

## 5. The structural fix: deliver off-TTY, keep the pane for the human

§4 hardens the racy channel; this section *removes* it. The principle is the one
Agent Teams encodes: **separate the transport from the display.** The human needs
the pane (scrollback + keyboard). The *message* does not need the pane — it needs
to arrive as a fresh user turn. Decouple them.

### 5.1 The mechanism: an agent-drained control channel via UserPromptSubmit

Claude Code's `UserPromptSubmit` hook can emit
`hookSpecificOutput.additionalContext`, which the docs describe as *"wrapped in a
system reminder and inserted into the conversation"* — the model reads it on its
next turn. Wire delivery as a *pull the agent performs*, not a *push the broker
types*:

1. Broker writes a framed record to `$STATE/ctrl/<agent>.fifo` (or appends to
   `$STATE/inbox/<agent>.log`) — **the broker never touches the TTY.**
2. On the agent's next turn boundary, a `UserPromptSubmit` (or `Stop`) hook drains
   pending records and emits them as `additionalContext`.
3. The existing `UserPromptSubmit` event in `events.jsonl` *is* the ACK — the same
   signal `delivery.cpp::scanEvents` already consumes (lines 365-397).

This eliminates **all three races of §1 at once**: no line-editor state to infer
(so `pane.cpp`'s 200 lines of mode/buffer scraping become irrelevant to
delivery), no keyboard contention (the human types into the pane; the broker
writes a file), no mid-stream interleave (the hook fires at a clean turn
boundary, not mid-tool-chain). The pane survives untouched for human attach. This
is the survey's recommendation #1, and Agent Teams is the existence proof that it
is the correct shape.

### 5.2 The boot-strap problem and the Monitor variant

The honest cost (well captured in our own `docs/delivery-alternatives.md`): a
pull channel requires the agent to *start consuming* before delivery works. Three
agent-side mechanisms, by robustness:

- **Hook-drained FIFO (recommended).** The `UserPromptSubmit` hook is configured
  in `settings/claude-settings.json` and runs unconditionally every turn — there
  is no agent action to forget. Cost: delivery only lands when the agent *takes a
  turn*. A truly idle agent (no turns) needs a nudge to wake it — which can be a
  single, minimal, *signal-only* TTY write (one keystroke, no payload), shrinking
  the racy surface from "the whole message" to "a doorbell."
- **`Monitor` tool on `tail -F $STATE/inbox/<agent>.log`** (our doc's option B).
  Each new line arrives as a queued harness notification — same path `/loop`
  output uses, crosses mid-tool-call boundaries cleanly. Cost: the agent must
  *invoke* Monitor at SessionStart; a missed invocation is a silent black hole.
  This is exactly Agent Teams' "automatic delivery" feel, but the agent owns the
  consumer.
- **`/loop NN bus msg fetch inbox-<self>`** (option C). Pure poll. Latency = the
  interval; cost = a full turn per tick even when empty, with cache-TTL
  interactions (>5 min interval = every fire cache-cold). Worst latency/cost, but
  zero new infrastructure.

### 5.3 Why our own doc lands on the hybrid — and where it under-reaches

`docs/delivery-alternatives.md` (elodin) recommends **D: push happy-path +
`/loop` fallback**, ~20-30 LOC. That's a defensible *incremental* step and
correctly notes push failures degrade to latency, not data loss. But it preserves
the racy push as the *primary* path — it hardens around the fragility rather than
removing it. The stronger long-term target inverts the default: **off-TTY pull as
primary (§5.1), with a minimal signal-only TTY nudge as the wake mechanism, not
the payload carrier.** The bracketed-paste swap (§4) is the right move for the
*nudge* and for the human-typed `bus msg send` path that must stay TTY-shaped.

---

## 6. How this maps to claude-bus — adopt / change, with flaws spotted

Principle-first, each with rough effort/payoff. Flaws in our current code carry
file refs.

### 6.1 Swap `write-chars` → `paste` (bracketed) — Low / High

The cheapest real win. `pane.cpp::sendToPane` (lines 518-519) streams characters,
which can line-split multi-line mail and races the editor harder than a framed
paste. zellij's `paste` action exists, takes `--pane-id`, and wraps in
`ESC[200~/201~`. ~4 lines, reversible, removes the multi-line-split class. Do this
first; it de-risks every TTY write we keep.

### 6.2 Off-TTY delivery via UserPromptSubmit-drained FIFO — Med / Very High

The structural fix (§5.1). Move `agent-inbox` delivery off
`deliverInline`→`sendToPaneSafe` onto a broker-writes-FIFO / hook-drains-and-emits-
`additionalContext` path. Keep the existing `UserPromptSubmit` ACK in
`scanEvents`. Prototype on one agent (e.g. elodin) behind a per-agent flag before
fleet-wide. This is what makes ~400 lines of `pane.cpp` + `dispatch.cpp` screen-
scraping *delivery-irrelevant*.

### 6.3 Reserve headless `stream-json` for headless-by-nature work — Low / Med

Don't headless-ify fleet agents (kills attach). *Do* use
`claude -p --input-format stream-json` for cron/reflection/judge jobs where the
`result`-message ACK and zero TTY race are pure upside and there's no human surface
to lose (§3.4). Aligns with the survey's cron-agent recommendation.

### 6.4 Steal amux's replay-on-restart for our recovery path — Med / High

Our `scanEvents` SessionEnd handler (`delivery.cpp` lines 306-348) releases
in-flight records **without advancing the cursor** so a respawned session re-finds
them — that *is* replay-on-restart, and it's correct. The gap: it's passive
(waits for a *new* dispatch tick). amux makes recovery *active* — on detecting
corruption/restart it re-sends immediately. Pair this with §6.5's idempotency key
so active replay can't double-act.

### 6.5 Add an idempotency key to the agent-side drain — Low / High

Any retrying transport is at-least-once; the §5.1 pull path and §6.4 active replay
both can redeliver. Records already carry `msg_id`. Have the drain hook record the
last-seen `msg_id` per topic and refuse to re-emit a seen id. Closes the
double-injection hole the survey flagged. ~15 LOC in the hook.

### 6.6 Flaws spotted in the current transport code

- **`mode=unknown` defers forever on a scrolled pane.** `pane.cpp::sendToPaneSafe`
  (line 557) returns `false` when the footer isn't visible; if the human leaves a
  pane scrolled up, delivery stalls until the ACK-timeout retry loop escalates to
  `inbox-ops` (60 s × 3). Off-TTY delivery (§6.2) removes the screen-scrape
  dependency entirely. Until then, this is a real latency-tail and false-defer
  source the doc's own notes acknowledge.
- **Synchronous zellij subprocesses block the single delivery thread.**
  `pane.cpp` runs every `dump-screen`/`write-chars`/`paste` as a forked child on
  the broker's main `pselect` thread with a 5 s SIGKILL timeout (lines 33-59).
  `dispatchTuiCommands` warns it "can take up to ~5s on retries" and stalls that
  topic. A hung zellij IPC therefore freezes `scanEvents` (ACKs stop draining) and
  `scanRetries` — the "broker delivery wedge" in memory. Off-TTY delivery removes
  the per-delivery subprocess; the survey's epoll/inotify loop removes the
  blocking poll. Either is a real fix; both compound.
- **`detectMode` heuristic is one TUI-layout change from breaking.** The "bypass
  permissions" fallback (lines 354-366) and the `findInputLine`/`parseInput`
  cursor-on-suggestion fix (lines 433-436) are scar tissue from real wedges. This
  is *inherent* to screen-scraping a private layout and is the strongest argument
  for §6.2: structured ACK + off-TTY delivery makes the parser optional rather than
  load-bearing.
- **`flock` is advisory and protects only broker-internal writers.**
  `dispatch.cpp::FlockGuard` + `deliverInline`'s lock serialize the broker's own
  TTY writes, but the *human's keyboard* bypasses the lock entirely (it goes
  zellij→PTY directly). The presence sentinel (`hasPresenceFile`) is the real
  human-contention guard, not the flock — worth documenting so nobody assumes the
  lock prevents human/broker interleave. It doesn't; it can't.
- **The `i` + `Ctrl-U` normalize mutates the human's draft on every delivery.**
  `sendToPaneSafe` (lines 592-606) clears the line then restores the saved draft.
  If the human is mid-keystroke during the ~250 ms window of `i`/`Ctrl-U`/write/
  restore, their input and the broker's interleave despite the flock (see prior
  bullet). Off-TTY delivery eliminates this; bracketed-paste shrinks the window
  (no per-char streaming) but doesn't close it.

---

## 7. Synthesis — the sequence

1. **`paste` over `write-chars` (§6.1).** This week. Low effort, removes
   multi-line-split, de-risks every retained TTY write, fully reversible.
2. **Off-TTY delivery: FIFO drained by `UserPromptSubmit` (§6.2).** The structural
   move. Keeps the pane for the human (the moat), deletes the line-editor and
   keyboard races, makes the screen-scraper optional. Prototype on one agent.
3. **Idempotency key on the drain (§6.5).** Ships *with* #2; makes at-least-once
   safe.
4. **Active replay-on-restart (§6.4).** Hardens recovery; amux-proven.
5. **Headless `stream-json` for cron/judge jobs (§6.3).** Right tool for
   no-human-surface work; wrong tool for the fleet.

The unifying principle, stated once: **transport and display are different
concerns.** The pane is the human's; the message belongs off-TTY. Agent Teams
proves the shape, amux proves the recovery discipline, and zellij's own `paste`
action is a free first step we already have installed.

---

## Sources

**Claude Code official / programmatic input**
- [Orchestrate teams of Claude Code sessions (Agent Teams)](https://code.claude.com/docs/en/agent-teams) — inbox/SendMessage transport, tmux/iTerm as *display* mode, TeammateIdle/TaskCreated/TaskCompleted hooks, file locking for task claims
- [Agent Teams message-routing bug — claude-code#58762](https://github.com/anthropics/claude-code/issues/58762) — "transport is NOT TTY send-keys… filesystem-based message inbox that agents poll"
- [Agent SDK overview (sessions, resume, init/result messages)](https://code.claude.com/docs/en/agent-sdk)
- [`--input-format stream-json` undocumented usage — claude-code#24594](https://github.com/anthropics/claude-code/issues/24594)
- [CLI stream-json protocol (community) — Roasbeef/claude-agent-sdk-go](https://github.com/Roasbeef/claude-agent-sdk-go/blob/main/docs/cli-protocol.md) — control_request/response, request_id, result fields
- [Wrapping the Claude CLI (stream-json NDJSON example) — avasdream](https://avasdream.com/blog/claude-cli-agentic-wrapper)
- [Claude Code hooks reference](https://code.claude.com/docs/en/hooks) — UserPromptSubmit additionalContext semantics

**Comparable implementations**
- [amux — agent multiplexer (tmux send-keys + ANSI-scrape, self-healing watchdog, replay-last-message)](https://github.com/mixpeek/amux)
- [Tmux-Orchestrator send-claude-message.sh (send-keys; sleep 0.5; Enter)](https://github.com/Jedward23/Tmux-Orchestrator)

**zellij input mechanics**
- [zellij CLI actions (write-chars / write / paste / send-keys)](https://zellij.dev/documentation/cli-actions)
- [zellij `cli.rs` action enum (Paste = bracketed paste; all take --pane-id)](https://github.com/zellij-org/zellij/blob/main/zellij-utils/src/cli.rs)
- [zellij mouse & paste handling — bracketed-paste ESC[200~/201~ implementation](https://deepwiki.com/zellij-org/zellij/6.2-mouse-and-paste-handling)
- [zellij programmatic control / paste "faster and more robust than write-chars"](https://zellij.dev/documentation/programmatic-control.html)

**PTY / terminal mechanics**
- [pty(7) man page (write-to-master delay; no interleave guarantee)](https://www.man7.org/linux/man-pages/man7/pty.7.html)
- [TIOCSTI disabled by default since Linux 6.2 (dev.tty.legacy_tiocsti)](https://www.man7.org/linux/man-pages/man2/TIOCSTI.2const.html)
- [Multi-line paste line-splitting (bracketed-paste loss) — openclaw#18809](https://github.com/openclaw/openclaw/issues/18809)

**Our own code & prior analysis**
- `src/pane.cpp`, `src/delivery.cpp`, `src/dispatch.cpp`, `settings/hooks/log-event.sh`, `settings/claude-settings.json`, `layouts/fleet.kdl`
- `docs/delivery-alternatives.md` (elodin) — push vs Monitor vs /loop vs hybrid
- `docs/modern-agent-techniques.md` §1.1 — the survey-level transport thesis
