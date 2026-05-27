# Human ↔ Agent Direct Conversation: Focus-Aware Mailbox

> **SUPERSEDED.** This doc pitches focus-as-presence with the file as
> override. The implementation chose **sentinel-only**: presence is
> exclusively `<state>/presence/<name>`, written by `[bus-attach]` /
> removed by `[bus-detach]`. Focus is observed but never gates bus
> behavior — sulin must be able to watch a pane without halting its
> autonomous flow. See `CLAUDE.md` and the project memory
> `attach-as-presence` for the live model. The design pressure and
> mechanism descriptions below are still useful background; treat
> their conclusions as one revision behind the code.

## Headline

**Treat zellij focus as presence.** The watcher suppresses its
`[bus-wake]` nudge for the focused agent; the drain hook replaces mail
injection with a one-line banner. A zellij chord (`Ctrl+G a` / `d`)
types a sentinel into the focused pane that the same hook intercepts,
giving an explicit override when focus alone is wrong. A new
`bin/presence-bar` strip on agent tabs makes the state visible. Total
change: roughly fifty lines across the watcher, the hook, a new tiny
binary, and the layouts. No new daemons, no floating panes, no
foreground attach process.

## The problem, sharply stated

The bus today has one channel into an agent: the TUI prompt buffer.
Three writers contend for it:

1. **sulin's keyboard** when typing into a pane
2. **the watcher**, which writes `[bus-wake]` into the buffer whenever
   peer mail appends to the agent's log
3. **the `UserPromptSubmit` drain hook**, which prepends the
   `## bus mailbox` block to whatever prompt submits

When sulin is mid-conversation with elodin and bast mails a review
request, every one of those writers fires into the same channel. The
watcher's `[bus-wake]` lands in sulin's input box; the drain hook
prepends bast's message to whatever sulin happened to be typing;
elodin receives a tangled prompt and can't tell which thread to
answer. The mailbox content isn't lost — it's delivered into the
middle of an unrelated conversation, and the answer that comes back
is half-pulled toward the wrong sender.

The framing sulin gave is the right one: separate **"I am interacting
with the human"** from **"I need to ping a message over."** The
system has only one channel today; it needs a presence signal so the
mail layer can defer.

## Recommendation

Three moving parts that compose:

### 1. Focus is presence (default)

`zellij action list-clients` returns one line per attached client with
the focused pane id. Resolve pane id → agent name via the existing
title lookup. Both the **watcher** and the **drain hook** stat this
before acting on mail:

- **Watcher:** before each `nudge(recipient)`, check if any client is
  focused on the recipient's pane. If so, skip the TUI write. Still
  update `last_size` so we don't fire a backlog of suppressed nudges
  when focus moves.
- **Drain hook:** same check. If the agent is focused, run `peek`
  (non-consuming) and emit a single-line banner —
  `## bus: 3 message(s) queued (you're attached; run bin/mailbox drain
  to see)` — and exit. Mail stays in the queue.

When focus moves away, the watcher's next inotify event nudges
normally; the agent's next turn drains everything that accumulated.

Cost: one subprocess per inotify event and per `UserPromptSubmit`.
Tens of milliseconds. No state file to forget, no command to
remember. Focus is the truth.

### 2. Sentinel keybinding (explicit override)

Sometimes focus is wrong — sulin reads bob's pane in a separate tab
while protecting an in-flight elodin conversation. For that, a
zellij chord types a sentinel into the focused pane; the existing
hook recognizes it and acts.

`~/.config/zellij/config.kdl`:

```kdl
keybinds {
    shared_except "locked" {
        bind "Ctrl g" { SwitchToMode "bus"; }
    }
    bus {
        bind "a" {
            WriteChars "[bus-attach]"; SendKey "Enter";
            SwitchToMode "normal";
        }
        bind "d" {
            WriteChars "[bus-detach]"; SendKey "Enter";
            SwitchToMode "normal";
        }
        bind "Esc"    { SwitchToMode "normal"; }
        bind "Ctrl g" { SwitchToMode "normal"; }
    }
}
```

`Ctrl+G a` → types `[bus-attach]\n` into the focused pane. The
`UserPromptSubmit` hook sees the sentinel, writes
`<state>/presence/$CLAUDE_BUS_AGENT_ID`, prints a status line on
stderr, and exits with code 2 (blocks the prompt from reaching the
model). Zero token cost, no new pane, no floating flicker. `Ctrl+G d`
mirrors with `[bus-detach]`: removes the file, fires one `[bus-wake]`
to drain anything queued, blocks the prompt.

The presence file is the **override**: both the watcher and the hook
treat presence as `focused OR file-exists`. Focus is the default
signal; the file is the override.

### 3. Presence bar on agent tabs

A third `size=1 borderless` row in the agent tab template runs
`bin/presence-bar`, a small C++ binary that renders one line:

```
alice ●attached   bob ◐focus   elodin ○idle (2 unread)   kvothe ○working
```

- `●` — explicit attach (file present)
- `◐` — focus-derived only (no file)
- `○` — neither

Same idiom as the existing tab-bar/status-bar rows. Refresh every
second. Reads `<state>/presence/*` and `zellij action list-clients`;
counts unread via `bus::peek`.

## Why this shape

It mirrors **Slack's split between presence and DND**: presence is "am
I logged in?", DND is "should you notify me?" In the bus, the pane is
always present (the agent process is running); focus + presence file
together act as DND. Messages land either way; the only thing that
changes is whether they push.

It preserves the **doorbell-vs-content distinction the system already
has**. `[bus-wake]` is the doorbell; the mailbox holds the content.
Presence-aware suppression means the doorbell waits but the content
keeps stacking — exactly the deferred-notification model Slack uses
when DND is on. `[bus-attach]` and `[bus-detach]` extend the
sentinel idiom we already speak: another doorbell, eaten by the same
hook.

It's the **smallest change that fixes the race**. No new daemon, no
new IPC, no out-of-band channel. The watcher already stats files;
one extra subprocess (`list-clients`) per event is negligible at our
agent counts. The hook already shells out; one stat call plus a
prefix match is free. The keybinding reuses the WriteChars action
that zellij already exposes, and the hook intercept reuses the
`exit 2` semantics that Claude Code already documents.

## Concrete answers

**How does the agent know a human is direct-attached?**
`list-clients` (focus) by default; `<state>/presence/<agent>` (file
written by the sentinel hook) as override. Both signals OR'd. No
heuristics on input buffers or typing cadence — those race the
watcher and require expensive `dump-screen` calls.

**What changes for the mailbox?**
Queue silently; banner-only on prompt submit. No TUI nudge from the
watcher; no `## bus mailbox` content injection by the hook. The
unread count surfaces once per prompt as a one-liner; the rest of
the prompt context is sulin's.

**Where does the change live?**
- `src/bin/watcher.cpp` — `list-clients` + presence-file guard
  before `nudge()`.
- `settings/hooks/drain-mailbox.sh` — sentinel dispatch (`[bus-attach]`
  / `[bus-detach]`) plus the same guard for the drain path.
- `bin/presence-bar` — new C++ binary, same shape as `bin/monitor`.
- `layouts/agent-test.kdl` (and `bin/spawn-agent`) — new `agent_tab`
  template with the presence-bar row.
- `~/.config/zellij/config.kdl` — `bus` mode + `Ctrl+G a` / `d`
  chord.

State path follows the existing `<state>/mailbox/` convention:
`<state>/presence/<name>`, single file per agent, mtime = attach
timestamp.

**How does the human exit cleanly?**
Move focus to another pane — focus-derived presence vanishes
immediately. For an explicit attach, `Ctrl+G d` removes the file and
fires one final `[bus-wake]` so anything queued during the
conversation drains in the next turn. Belt-and-suspenders: presence
files older than one hour are treated as absent, so a forgotten
attach doesn't silence the agent forever.

**Agent → human surface today is mailing `human`. Replace or
complement?**
**Complement.** The two channels do different jobs:

| Channel | Direction | Cadence | Role |
| --- | --- | --- | --- |
| `bin/mailbox send human ...` + `bin/inbox human` | agent → human | async, persistent | the bus's "outbox" for status, summaries, results |
| Focus + sentinel + type at pane | human ↔ agent | sync, real-time | the bus's "voice call" — pair programming, teaching, debugging |

Direct conversation is the inverse-direction, real-time analog of
the inbox pane. Both run concurrently: sulin can be focused on
elodin teaching binary formats while the inbox pane fills with
status pings from autonomous peers in other panes.

## Alternatives considered

### Floating pane keybinding

Earlier draft: chord opens a floating pane that runs
`bin/bus attach`. Rejected because the sentinel trick avoids the
flicker, the focus theft, and the "which pane was sulin on before the
floating pane stole focus?" disambiguation problem. The sentinel
lands in the pane sulin was already typing in; `$CLAUDE_BUS_AGENT_ID`
in the hook env answers "who is being claimed" without any
inference.

### Pane-state heuristic

Use `bin/pane-state NAME` to detect input-buffer content or typing
cadence; treat non-empty buffer as attached.

- **Pro:** zero ceremony.
- **Con:** empty buffer ≠ not present (sulin might be reading
  scrollback). Requires a `dump-screen` per check — measurable cost.
  Races sulin's keystrokes against the watcher's nudge either way.
- **Verdict:** rejected. `list-clients` is a cleaner native signal.

### Foreground `bin/bus attach` process

A blocking command that writes the presence file and waits for
SIGINT.

- **Pro:** explicit and self-documenting.
- **Con:** sulin has to remember to run it; consumes a shell. Adds a
  process to manage. The keybinding chord covers the same need with
  less ceremony.
- **Verdict:** rejected as primary. A `bin/bus attach NAME` CLI is
  still worth shipping as a scriptable entry point (e.g., for
  tooling that wants to programmatically claim presence), but it's
  not the user-facing path.

### Channel separation in the prompt

The drain hook reads stdin and could detect `[bus-wake]` exactly,
draining only on the doorbell.

- **Verdict:** now incorporated. The hook's sentinel dispatch
  (`[bus-wake]` → drain, `[bus-attach]` → claim presence,
  `[bus-detach]` → release, anything else → presence-aware behavior)
  is exactly this idea generalized.

### Priority override (iOS interruption levels)

Borrow Apple's Passive / Active / Time-Sensitive / Critical
taxonomy; let `priority >= 200` mailbox messages bypass presence.

- **Verdict:** deferred. The mailbox already carries `priority u8`
  but no current sender sets it. Defining "critical" without traffic
  to motivate it picks the threshold blind. Add when a real peer
  needs to break through.

## Prior art

### Slack DND

Slack's central design choice: **presence and notifications are
orthogonal**. The green dot says "I'm logged in"; DND says "don't
push me." Messages still land in channels and DMs during DND — they
just don't ring. Senders see a 🔕 indicator and can choose "notify
anyway" through a friction-laden override reserved for emergencies.
([Slack DND help](https://slack.com/help/articles/214908388-Pause-notifications-with-Do-Not-Disturb))

Maps directly onto the bus: the agent is always online (the pane
exists); focus or presence file = DND on; banner = the 🔕 indicator;
the eventual priority-override = the "notify anyway" button.

### iOS Focus & interruption levels

Apple's 2021 Focus framework added four notification interruption
levels: **Passive, Active, Time-Sensitive, Critical**. Passive
doesn't even light up the screen; Critical bypasses all device
controls. Time-Sensitive is the interesting middle — it breaks
Focus only with explicit per-app permission.
([OneSignal: iOS focus modes](https://documentation.onesignal.com/docs/en/ios-focus-modes-and-interruption-levels))

The right model for any future "let urgent peers through attach
mode" extension, and the framework for thinking about expanding the
presence file from a flag to a tier.

### XMPP `<show>` states

XMPP standardized presence as `chat | away | dnd | xa` plus a
free-form `<status>` ([XEP-0085](https://xmpp.org/extensions/xep-0085.html)).
Four states is more than we need today; one bit suffices. The
natural extension: the presence file could later store
`attached | away | working`, with hook behavior keyed off the value.

### IRC `/AWAY`

IRC's minimum viable version: `/AWAY [message]` toggles a flag;
PRIVMSGs auto-reply with `RPL_AWAY` and your message ([RFC 2812](https://datatracker.ietf.org/doc/html/rfc2812)).
Total state: one flag, one string. Senders aren't blocked, messages
still arrive — the away notice is courtesy, not gating. Our
presence file is structurally identical.

### Call-center barge / whisper / listen

The supervisor monitoring vocabulary — **Listen** (silent observer),
**Whisper** (one-way to agent only, caller doesn't hear), **Barge**
(3-way) ([OnSIP](https://www.onsip.com/voip-resources/smb-tips/call-monitoring-features-monitor-whisper-and-barge-explained))
— is the cleanest taxonomy for what direct conversation actually
is. Today's "type at the pane" is **Barge**: every observer sees
what sulin types and what the agent answers. Peers mailing during a
Barge are trying to **Whisper** but only have the Barge channel
available. The presence-aware mailbox gives us a Whisper-equivalent
for the bus.

### Game NPC dialogue interruption

Two patterns to steal. **KOTOR's "X wants to talk" prompt** surfaces
that an NPC needs attention without yanking the player out of
current dialogue. **Oxenfree's resumed dialogue** has NPCs re-enter
interrupted threads with explicit acknowledgment ("Anyway —", "So…
Yeah —")
([modes.io: NPC conversation](https://modes.io/observations-about-npc-conversation/)).

The banner is the first pattern. The second is the right voice for
the post-detach catch-up message: when focus moves away (or `Ctrl+G d`
fires), the agent's next turn could lead with `## bus mailbox (queued
during attach; 3 items)` — the verbal equivalent of "Anyway —."

### tmux multi-client attach

tmux's "two clients on one session" model is the closest analog to
direct conversation: both parties type to the same PTY, both see the
same output. The known failure mode is exactly ours — keystrokes
interleave when both type at once
([hamvocke: pair programming with tmux](https://hamvocke.com/blog/remote-pair-programming-with-tmux/)).
The convention in practice is social rather than technical, which
works for two humans and degrades fast with N peers (the bus).
Focus-as-presence is the social convention made explicit, enforced
by the watcher and the hook.

## Sharp edges

1. **Race on focus change.** Window: sulin moves focus from alice to
   bob → mail arriving at alice in that exact window doesn't get a
   nudge (focus check said "still attached"). **Fix:** none needed.
   The next mail event fires a nudge under the new focus state, and
   that nudge drains everything pending. The race only delays delivery
   by one inter-mail interval; nothing is lost.

2. **Stale presence files.** Forgotten attach silences the agent
   forever. **Fix:** drain hook and watcher treat presence files older
   than one hour as absent. Generous default; tunable.

3. **Chord typed in the wrong pane.** `Ctrl+G a` from a bash shell
   types `[bus-attach]\n` into the shell. Harmless garbage — `bash:
   command not found`. No state changes. If sulin does it inside a
   non-bus claude pane (no `CLAUDE_BUS_AGENT_ID`), the hook's first
   guard exits cleanly.

4. **Visible flicker in the input box.** `WriteChars "[bus-attach]"`
   makes the text appear in claude's input field for one frame before
   the hook eats it. Single-frame visual; acceptable in practice.

5. **`list-clients` cost.** ~10 ms per fork. At one watcher event +
   one UserPromptSubmit per agent turn, negligible. If it becomes a
   bottleneck (many agents, chatty mailbox), cache the parse for
   100 ms — focus rarely changes faster than that.

6. **Multiple clients on one session.** Two zellij clients both
   focused on different panes: both are "present" simultaneously.
   That's the correct semantics — if anyone's looking, suppress the
   nudge. No special handling needed.

7. **Watcher logs suppressions.** `watcher: alice grew 192 -> 296,
   suppressed (focus)` or `(presence)`. One log line variant; useful
   for the "why isn't alice waking up?" debugging session.

## Implementation sketch

### Watcher (`src/bin/watcher.cpp`)

Add a presence check before `nudge(recipient)`:

```cpp
auto isPresent(const std::string& recipient) -> bool {
  // Cheap path first: presence file (explicit attach).
  const auto pf = stateRoot() / "presence" / recipient;
  if (std::error_code ec; fs::exists(pf, ec)) {
    using namespace std::chrono;
    const auto age = file_clock::to_sys(fs::last_write_time(pf))
                         .time_since_epoch();
    if (system_clock::now().time_since_epoch() - age < hours{1}) {
      return true;
    }
  }
  // Focus path: list-clients, match recipient's pane id.
  const auto pane_id = panes::resolve(recipient);  // existing logic
  if (pane_id.empty()) return false;
  // Fork `zellij action list-clients`, scan for pane_id.
  return clientFocusedOn(pane_id);
}
```

Then in the inotify loop:

```cpp
if (sz > last && sz > kHeaderBytes) {
  if (isPresent(recipient)) {
    std::println("watcher: {} grew {} -> {}, suppressed (presence)",
                 recipient, last, sz);
  } else {
    std::println("watcher: {} grew {} -> {}, nudging", recipient, last, sz);
    nudge(recipient);
  }
}
```

### Drain hook (`settings/hooks/drain-mailbox.sh`)

```bash
#!/usr/bin/env bash
set -euo pipefail

MAILBOX=/home/sulin/claude-bus/bin/mailbox
BUS=/home/sulin/claude-bus/bin/bus
STATE=${CLAUDE_BUS_STATE:-/tmp/claude-bus}

agent="${CLAUDE_BUS_AGENT_ID:-}"
[ -z "$agent" ] && exit 0

# Hook input is JSON on stdin; the prompt field carries what the user typed.
input=$(cat)
prompt=$(printf '%s' "$input" | jq -r '.prompt // ""')

case "$prompt" in
    "[bus-attach]")
        mkdir -p "$STATE/presence"
        date -u +%s > "$STATE/presence/$agent"
        echo "bus: attached" >&2
        exit 2  # block; never reaches the model
        ;;
    "[bus-detach]")
        rm -f "$STATE/presence/$agent"
        "$BUS" send "$agent" "[bus-wake]" >/dev/null 2>&1 || true
        echo "bus: detached" >&2
        exit 2
        ;;
esac

# Presence-aware drain: peek + banner if attached, full drain otherwise.
pf="$STATE/presence/$agent"
attached=0
if [ -f "$pf" ]; then
    age=$(( $(date -u +%s) - $(stat -c %Y "$pf") ))
    [ "$age" -lt 3600 ] && attached=1
fi
if [ "$attached" -eq 0 ]; then
    # Focus check: is any client focused on our pane?
    pane=$("$BUS" pane-id "$agent" 2>/dev/null || true)
    if [ -n "$pane" ] && zellij action list-clients 2>/dev/null \
        | awk -v p="$pane" 'NR>1 && $2==p {found=1} END {exit !found}'; then
        attached=1
    fi
fi

if [ "$attached" -eq 1 ]; then
    count=$("$MAILBOX" peek "$agent" 2>/dev/null \
            | grep -c '^=== ' || true)
    [ "$count" -gt 0 ] && \
        printf '## bus: %d message(s) queued (you are attached; run bin/mailbox drain to see)\n' "$count"
    exit 0
fi

out=$("$MAILBOX" drain "$agent" 2>/dev/null || true)
[ -n "$out" ] && printf '## bus mailbox (auto-drained on prompt)\n\n%s\n' "$out"
```

### Presence bar (`src/bin/presence_bar.cpp`)

New binary, same shape as `monitor`:

```cpp
// Render one line per render-tick:
//   alice ●attached   bob ◐focus   elodin ○idle (2 unread)
//
// Refresh every 1s, line-buffered, no alt-screen. Each agent:
//   - read <state>/presence/<name> for attach state
//   - parse `zellij action list-clients` for focus-derived state
//   - bus::peek for unread count
// Sleep + re-render. Same render-with-clear-to-EOL idiom as monitor.
```

### Layouts (`layouts/agent-test.kdl`, `bin/spawn-agent`)

```kdl
layout {
    tab_template name="agent_tab" {
        pane size=1 borderless=true { plugin location="tab-bar" }
        children
        pane size=1 borderless=true \
            command="/home/sulin/claude-bus/bin/presence-bar"
        pane size=1 borderless=true { plugin location="status-bar" }
    }
    default_tab_template { /* unchanged — no presence row */ }

    agent_tab name="agents" focus=true {
        pane name="alice" command="bash" {
            args "-c" "CLAUDE_BUS_AGENT_ID=alice exec claude --dangerously-skip-permissions"
        }
    }
    tab name="ops" { /* ... */ }
}
```

`bin/spawn-agent` switches its layout-string to use `agent_tab`.

### Zellij config (`~/.config/zellij/config.kdl`)

```kdl
keybinds {
    shared_except "locked" {
        bind "Ctrl g" { SwitchToMode "bus"; }
    }
    bus {
        bind "a" {
            WriteChars "[bus-attach]"; SendKey "Enter";
            SwitchToMode "normal";
        }
        bind "d" {
            WriteChars "[bus-detach]"; SendKey "Enter";
            SwitchToMode "normal";
        }
        bind "Esc"    { SwitchToMode "normal"; }
        bind "Ctrl g" { SwitchToMode "normal"; }
    }
}
```

## What this opens up later

- **Multi-state presence.** Replace the timestamp in the presence
  file with an XEP-0085-style word: `attached`, `busy`, `away`. Hooks
  key behavior off the word. Presence bar shows it.
- **Priority override.** Honor `priority >= N` in the watcher's
  presence check. Maps to iOS Time-Sensitive.
- **Presence column in `monitor`.** The dashboard's `STATE` column
  already has `IDLE / HAS_MAIL / WORKING / STUCK`; add `ATTACHED`
  derived from the same focus+file OR.
- **Per-peer "do not disturb."** Extend the presence file to a small
  table: `attached except [alice, bob]` — allow specific peers
  through. iOS Focus's per-contact allowlist.

None of these need to ship now. The point is that the focus-as-default
+ sentinel-as-override shape composes; the path to richer behavior
doesn't require redesigning the core.

## Sources

- [Slack: pause notifications with DND](https://slack.com/help/articles/214908388-Pause-notifications-with-Do-Not-Disturb)
- [Slack help: customize notifications](https://slack.com/resources/using-slack/customize-your-notifications-in-slack)
- [iOS Focus modes & interruption levels (OneSignal)](https://documentation.onesignal.com/docs/en/ios-focus-modes-and-interruption-levels)
- [XEP-0085: Chat State Notifications](https://xmpp.org/extensions/xep-0085.html)
- [XMPP Instant Messaging & Presence (RFC 3921)](https://xmpp.org/rfcs/rfc3921.html)
- [RFC 2812: IRC Client Protocol](https://datatracker.ietf.org/doc/html/rfc2812)
- [Call monitoring modes: monitor / whisper / barge (OnSIP)](https://www.onsip.com/voip-resources/smb-tips/call-monitoring-features-monitor-whisper-and-barge-explained)
- [modes.io: Observations About NPC Conversation](https://modes.io/observations-about-npc-conversation/)
- [hamvocke: Remote pair programming with tmux](https://hamvocke.com/blog/remote-pair-programming-with-tmux/)
- [Zellij: keybindings & possible actions](https://zellij.dev/documentation/keybindings-possible-actions)
- [Claude Code: hook reference](https://code.claude.com/docs/en/hooks)
