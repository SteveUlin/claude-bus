# Mailbox Design Space: A Total Design

## Headline

**Split the metadata into operational flags (transport acts on them) and a
semantic protocol tag (consumer interprets them). Add a per-pane state-
aware serializer so TUI-bound dispatch is reliable, not just optimistic
"send keys" into a PTY that may not be in a state to receive them. Keep
one ordered log per recipient; let verbs sugar over the common flag
combinations.** The result: the drain hook is ten lines with no
per-type exceptions, slash dispatch becomes part of a general "drive
the pane" mechanism that future drivers (bash, vim, headless processes)
plug into, and the mailbox stays a pure transport that other layers —
task queues, RPC, blackboards — build on without renegotiating the
wire format.

## How we got here

Three positions in tension, ordered by depth of intervention:

- **A. Generic primitive** — one typed-record log per recipient; type byte
  routes by application protocol. Today's shape: type=0 text, type=1
  slash. Optimizes infrastructure reuse; sacrifices abstraction
  clarity, because the drain hook accumulates exception clauses
  whenever two types want different transport behavior (the existing
  `if has_slash → drain everything → dispatch` patch is the visible
  scar).
- **B. Conversation channel** — mailbox carries text only; every other
  shape (slash dispatch, tasks, RPC, broadcast) gets its own primitive.
  Optimizes semantic clarity; sacrifices shared substrate.
- **C. Split verbs, shared substrate** — keep one wire format but
  introduce a second verb (`bus tui`) for TUI-recipient traffic. Names
  the recipient process at the API; preserves reuse on the wire.

C is the right pull, but the verb split alone doesn't go far enough.
The remaining question — *what should the wire actually carry?* — is
where the operational/semantic distinction comes in. C plus the
distinction is this doc's total design.

## The deciding principle

Saltzer/Reed/Clark, 1984: place functions only at the level where they
can be implemented completely and correctly; everything else moves to
the endpoints ([the paper](https://web.mit.edu/saltzer/www/publications/endtoend/endtoend.pdf)).
The transport's job is to deliver records reliably and in order with
the right timing properties; understanding what those records *mean* is
the endpoints' job.

AMQP works exactly this way: `delivery_mode` is a broker-acted
property (persist to disk or hold in memory); `content_type` is broker-
ignored metadata ([AMQP messages](https://lavinmq.com/documentation/amqp-messages)).
CloudEvents formalizes the envelope as a stable transport-neutral
shape; the same event traverses HTTP, Kafka, or AMQP without re-typing
([CloudEvents Kafka binding](https://github.com/cloudevents/spec/blob/main/cloudevents/bindings/kafka-protocol-binding.md)).
ZeroMQ pushes the principle to its limit by removing semantic types
entirely — only socket patterns encode meaning ([ØMQ guide](https://zguide.zeromq.org/docs/chapter2/)).

Translated: the mailbox header should carry operational flags that the
drain, the serializer, and the watcher act on. Anything else —
"slash", "task", "rpc-request", "ping" — is a label the transport
never reads. The label travels in a separate field, free-form, so
adding a new protocol costs no wire change.

## Wire format (header v2)

```
┌────────────────────────────────────────────────────────────────────┐
│ ENVELOPE (transport reads for accounting, dedupe, ordering)         │
├────────────────────────────────────────────────────────────────────┤
│ record_len   : u64      record size; atomic-append boundary         │
│ sent_ms      : u64      wall-clock at send                          │
│ rand_tag     : u16      id uniqueness                               │
│ sender_len   : u8                                                   │
│ sender       : N bytes  CLAUDE_BUS_AGENT_ID                         │
├────────────────────────────────────────────────────────────────────┤
│ OPERATIONAL FLAGS (transport ACTS on these; the only vocabulary     │
│                    the drain / serializer / watcher branch on)      │
├────────────────────────────────────────────────────────────────────┤
│ target       : u8       0 = MODEL  inject into prompt context       │
│                         1 = TUI    dispatch to recipient's TTY      │
│                                    via the per-pane serializer      │
│                         2 = RAW    persist only; no delivery        │
│                                                                     │
│ merge_policy : u8       0 = MERGE     batch with siblings on drain  │
│                         1 = EXCLUSIVE consume alone; leave rest     │
│                                                                     │
│ presence_policy : u8    0 = DEFER    honor attach-as-presence       │
│                         1 = DISPATCH deliver regardless of attach   │
│                                                                     │
│ ttl_ms       : u32      0 = no expiry; else drop if sent_ms +       │
│                         ttl_ms < now at drain time                  │
├────────────────────────────────────────────────────────────────────┤
│ SEMANTIC METADATA (transport IGNORES; endpoints interpret)          │
├────────────────────────────────────────────────────────────────────┤
│ protocol_len : u8                                                   │
│ protocol     : N bytes  "text" | "task" | "rpc-req" | "rpc-reply"   │
│                       | "slash" | "ping" | …  (free-form)           │
│                                                                     │
│ priority     : u8       opaque consumer hint                        │
│ correlation  : u128     0 = none; non-zero for RPC pairs            │
├────────────────────────────────────────────────────────────────────┤
│ body_len     : u32                                                  │
│ body         : M bytes                                              │
└────────────────────────────────────────────────────────────────────┘
```

Overhead ~40 bytes plus sender plus protocol tag. Bodies still cap
near 4000 bytes under PIPE_BUF atomic append.

`kFormatVersion` bumps to 2. The existing reserved tail of the file
header is unchanged; only the per-record format grows. Records on
disk in v1 format are translated on read with the obvious mapping
(type==1 → target=TUI, merge=EXCLUSIVE, presence=DISPATCH, protocol="slash";
type==0 → target=MODEL, merge=MERGE, presence=DEFER, protocol="text"). In
practice the ephemeral state under `/tmp/claude-bus/mailbox/` is wiped
across reboots; the translator is belt-and-suspenders.

## Verb layer (sugar over flag combinations)

Callers pick the verb that names their intent; the verb encodes the
right operational defaults. Power callers override flags directly.

| Verb | target | merge | presence | ttl_ms | default protocol |
| --- | --- | --- | --- | --- | --- |
| `mailbox send NAME body` | MODEL | MERGE | DEFER | 0 | `text` |
| `mailbox task NAME body` | MODEL | EXCLUSIVE | DEFER | 0 | `task` |
| `mailbox ping NAME body` | MODEL | MERGE | DISPATCH | 10000 | `ping` |
| `mailbox rpc NAME body --reply-to ME` | MODEL | MERGE | DEFER | 30000 | `rpc-req` |
| `bus tui NAME body` | TUI | EXCLUSIVE | DISPATCH | 0 | `slash` |
| `mailbox audit NAME body` | RAW | — | — | 0 | `audit` |

Each verb is a thin CLI wrapper translating to `bus::SendOpts`. New
verbs are cheap — pick the flag combo and the default protocol; no
wire change. Custom protocols inherit a verb (`mailbox send NAME body
--protocol my-event`) or get their own sugar verb when patterns
solidify.

## Drain loop (one pass, no exceptions)

```
for each record past my cursor:
    if record.ttl_ms != 0 and record.sent_ms + record.ttl_ms < now_ms:
        advance cursor; continue                       # expired

    if record.presence_policy == DEFER and is_attached(self):
        break                                          # defer this and rest

    case record.target:
        MODEL:
            if record.merge_policy == EXCLUSIVE:
                emit alone with framing(record.protocol)
                advance cursor; return
            merge_buffer.append(record)
        TUI:
            serializer.enqueue(record.body)
        RAW:
            audit_log(record)

    advance cursor

if merge_buffer nonempty:
    emit "## bus mailbox" block, framing each by record.protocol
```

Notice what is *not* in this loop: no type peek, no `if has_slash`, no
"drain everything because something special is queued." Every routing
decision reads one operational flag.

## Reliability layer: state-aware dispatch (replacing "send keys")

`bus send` is a blind PTY write. Two ways it fails silently:

1. **Wrong mode**: claude TUI is in scrollback, a modal y/n confirmation,
   the autocomplete popover, or compaction. Bytes typed at the prompt
   don't reach the input field; the slash either does nothing or fires
   in the wrong context. The Claude Code repo itself documents this
   class of failure in tmux/send-keys scenarios — pane initialization
   races, the final Enter being dropped, 255-byte command truncation
   ([claude-code #23513](https://github.com/anthropics/claude-code/issues/23513),
   [#33987](https://github.com/anthropics/claude-code/issues/33987),
   [#42391](https://github.com/anthropics/claude-code/issues/42391)).
2. **Contention**: human keystrokes, watcher nudges, and a peer's
   `bus send` interleave at the PTY in any order; the input field
   becomes garbage.

The single-writer principle from the previous round addresses (2): one
serializer per pane owns all bus-side writes. The reliability layer
addresses (1): the serializer doesn't just write — it confirms the pane
is in a state where the write will land, normalizes if it isn't, and
verifies after.

The model is `pexpect` and tmux automation in production: don't blind-
write; wait for a known prompt, send, expect echo, retry with backoff
on failure ([pexpect docs](https://pexpect.readthedocs.io/en/stable/),
[5 serial automation gotchas](https://www.thegoodpenguin.co.uk/blog/5-serial-automation-gotchas/)).

### State machine per dispatch

```
       ┌────────────────────────────────────────────────────────┐
       │  serializer.dispatch(pane, body, expected_protocol):    │
       └──────────────────────────┬─────────────────────────────┘
                                  ▼
                ┌─────────────────────────────────┐
                │  detect_state(pane)              │
                │  via dump-screen + driver regex  │
                └────────┬───────────────────────┬┘
                         │                       │
                READY    │            not READY  │
                         ▼                       ▼
              ┌──────────────────┐    ┌───────────────────┐
              │ write(body)      │    │ normalize(pane)   │
              │ send-keys Enter  │    │ via driver        │
              │ (single write    │    │ (e.g. Esc Esc to  │
              │  through         │    │  dismiss modals)  │
              │  serializer →    │    └─────────┬─────────┘
              │  bus_send)       │              ▼
              └────────┬─────────┘    ┌───────────────────┐
                       ▼              │ detect_state again│
              ┌──────────────────┐    └─────────┬─────────┘
              │ verify(pane,     │              │
              │  body marker)    │     ┌────────┴────────┐
              │ via dump-screen  │ READY              still
              │ with timeout     │  ▼                  not
              └──┬───────────────┘  (loop)            READY
                 │                                     │
         success │             failure                 ▼
                 ▼                ▼          ┌───────────────────┐
        ┌──────────────┐   ┌─────────────┐   │ retry w/ backoff  │
        │ advance      │   │ retry up to │   │ (250ms, 1s, 4s)   │
        │ serializer   │   │ N times;    │   └─────────┬─────────┘
        │ cursor       │   │ on exhaust  │             │
        └──────────────┘   │ escalate    │     exhaust │
                           └─────────────┘             ▼
                                            ┌─────────────────────┐
                                            │ escalate:           │
                                            │  log to bus events; │
                                            │  mailbox send human │
                                            │   "[serializer]     │
                                            │   could not deliver │
                                            │   to NAME"          │
                                            │  leave in queue     │
                                            └─────────────────────┘
```

Concretely for the claude TUI driver:

- **detect_state**: dump-screen the last few lines; match the prompt
  marker (the `>` prompt with surrounding chrome). READY iff matched
  and no in-flight model response indicator visible.
- **normalize**: send `Esc Esc` to dismiss modals/autocomplete/
  scrollback. Cheap; idempotent in normal input mode.
- **write**: `write-chars BODY` followed by `send-keys Enter` (always
  pair — see the existing known issue with `\n` vs CR).
- **verify**: dump-screen; look for the trailing bytes of the body
  echoed in the input area, or for the prompt advancing to a new line
  indicating the message was accepted. 200 ms grace, then check.
- **escalate**: after N=3 retries, emit a structured log to
  `events.jsonl` and mail `human` with the failure summary; record
  stays in the queue (or moves to a dead-letter log) so a future
  attempt or human intervention can complete it.

### The Pane Driver interface (pane-is-the-agent generalization)

`detect_state`, `normalize`, `write`, `verify` are per-program. claude
TUI has its prompt patterns; bash has `$ ` or `# `; vim has modal
states with their own normalization. Codify the four operations as a
driver interface; ship the claude TUI driver in the first cut; let
future drivers slot in by name.

```cpp
struct PaneDriver {
  virtual auto detect_state(std::string_view pane_dump) -> State = 0;
  virtual auto normalize(zellij::PaneId pane) -> bool = 0;
  virtual auto write_body(zellij::PaneId pane, std::string_view body) -> bool = 0;
  virtual auto verify(zellij::PaneId pane,
                      std::string_view marker,
                      std::chrono::milliseconds timeout) -> bool = 0;
};

// Per-agent driver selection: env var CLAUDE_BUS_PANE_DRIVER,
// or layout config, or auto-detect from pane title prefix.
auto driver_for(const std::string& agent) -> std::unique_ptr<PaneDriver>;
```

The mailbox transport doesn't know anything about claude TUI — it
hands `(pane, body)` to the serializer, which dispatches through the
driver. The driver is the only place that contains TUI-specific
knowledge. Swap the driver, and the same mailbox addresses a bash
pane, a vim session, a headless `claude -p` reader-loop, or whatever
else lives in a pane. **The pane is the agent**; claude is one
implementation.

This is also where "more reliability than send-keys" lives concretely.
The driver owns: which prompt marker means READY, what dismisses
modals for this program, what counts as verified delivery, what TTL
makes sense for stale TUI commands. The transport stays agnostic.

## Worked examples

**Text mail.** `mailbox send elodin "thoughts on the cache?"`
Flags: `target=MODEL, merge=MERGE, presence=DEFER, ttl=0`, protocol=`text`.
If elodin is attached (presence file fresh), drain breaks early and
the record stays queued. Otherwise it appends to the merge buffer;
the drain emits the whole buffer as `## bus mailbox` on the next
prompt.

**Slash dispatch.** `bus tui worker "/clear"`
Flags: `target=TUI, merge=EXCLUSIVE, presence=DISPATCH, ttl=0`,
protocol=`slash`. The drain hook hands this to the serializer (target
is TUI, not MODEL). The serializer detects state on worker's pane,
normalizes if needed, writes `/clear`, verifies the prompt advances,
moves to the next record. Human attach state never enters the
decision because `presence=DISPATCH`.

**Task body.** `mailbox task worker "implement feature X"`
Flags: `target=MODEL, merge=EXCLUSIVE, presence=DEFER, ttl=0`,
protocol=`task`. Drained alone with framing `## task ({id}) from
primary: implement feature X`; other queued records wait for the next
turn. Natural one-task-per-turn semantics from the flag, no separate
hook mode.

**RPC.** `mailbox rpc kvothe "summarize last 3 turns" --reply-to elodin`
Flags: `target=MODEL, merge=MERGE, presence=DEFER, ttl=30000`,
protocol=`rpc-req`, correlation=`0x42…`. kvothe's drain frames as
`## rpc-request id=42 from elodin (reply via mailbox rpc-reply id=42):
summarize last 3 turns`. kvothe responds; a wrapper helper sends
`mailbox send elodin --protocol rpc-reply --correlation 0x42… body`.
At elodin's drain: framing matches against a pending-reply table by
correlation id.

**Heartbeat.** `mailbox ping cluster "alive"`
Flags: `target=MODEL, merge=MERGE, presence=DISPATCH, ttl=10000`,
protocol=`ping`. If the recipient was detached for 11s, the record
expired and the drain skips it. A stale "I am alive" is
misinformation, not information.

**Audit entry.** `mailbox audit ops "kvothe completed task 42"`
Flags: `target=RAW, …`. Persisted, never delivered. Lets the mailbox
double as a tamper-evident log for cross-cutting events that other
tooling reads later.

## What this fixes vs preserves

Fixes:
- Drain hook's `if has_slash → drain everything → dispatch` exception
  clause: gone. Pure flag-based dispatch.
- Blind PTY writes for TUI traffic: replaced with detect/normalize/
  write/verify/retry.
- Bus-vs-bus contention at the TTY: solved by single serializer per
  pane.
- Wire commitment per new protocol: gone. New protocols are a string
  in the `protocol` field plus a framing rule at the consumer.
- Lock-in to claude TUI: a future bash/vim/headless driver swaps in
  without touching the transport.

Preserves:
- Single per-recipient log → strict total order, no cross-channel
  ordering race.
- Atomic O_APPEND under PIPE_BUF → multi-producer safe.
- Attach-as-presence as a real invariant — now per-record via
  `presence_policy=DEFER` instead of a hook-side exception. Zellij
  focus alone is intentionally not a gate; only the `[bus-attach]`
  sentinel file suppresses defer-policy records.
- The `[bus-wake]` doorbell pattern.
- The mailbox-as-transport / tasks-as-layer-above split: the new
  `protocol` field is exactly where the task/RPC/heartbeat layering
  attaches without bleeding into the transport.

## MVP scoping — what to ship now vs later

The full design is the whole doc; the minimum committable change is
smaller. Slice it into three landings so the integration is testable
end-to-end at each step.

**Phase 1 — Wire and drain (mechanical).**
- `src/mailbox.{h,cpp}`: SendOpts and Message gain the operational and
  semantic fields. `kFormatVersion` → 2. Serialize/deserialize updated.
  v1 support dropped — reads against a v1 file fail loud; runtime
  state at /tmp/claude-bus is wiped on the cutover.
- `src/bin/mailbox.cpp`: replace `slash` subcommand with passthrough
  flags (`--target`, `--merge`, `--presence`, `--ttl`, `--protocol`).
  Add sugar verbs (`task`, `ping`, `rpc`, `audit`).
- `bin/bus`: add `tui NAME body` subcommand that calls
  `bus::send(NAME, body, {target=TUI, merge=EXCLUSIVE, presence=DISPATCH,
  protocol="slash"})`.
- `settings/hooks/drain-mailbox.sh`: rewrite as the flag-based loop.
  No type peek. No override clause.

After Phase 1, slash records still dispatch *via the drain hook
calling `bus send` directly* (the existing path) — the per-pane
serializer doesn't exist yet. This is identical behavior to today but
on the new wire; it's the safe migration point.

**Phase 2 — Per-pane state-aware dispatch.** Shipped as
`bin/dispatch-tui` (shell), simpler than a long-running daemon:

- `bin/dispatch-tui NAME BODY` acquires a per-pane `flock` on
  `$STATE/tui-locks/<name>.lock` (single-writer to the TTY — same
  property a daemon would give, without the daemon).
- Asks `bin/pane-state` whether the recipient pane is READY
  (mode=INSERT, buffer=(empty)). pane-state already does the ANSI-
  laced dump parsing; dispatch-tui reads its key:value output.
- If not READY: normalize via `Esc Esc i` (drop modal/visual, re-
  enter insert), brief sleep, recheck. Retry up to 3 times with
  exponential backoff (0.25s / 1s / 4s).
- If READY: write via `bin/send` (existing write-chars + send-keys
  Enter pair).
- On exhausted retries: structured event in `events.jsonl` and a
  mail to `human` so the failure surfaces — record stays consumed
  (or fans out to a dead-letter log in a follow-up).
- Drain hook (`settings/hooks/drain-mailbox.sh`) spawns dispatch-tui
  in the background via `nohup ... &` so the user's prompt doesn't
  block on retries. Multiple TUI records for the same pane serialize
  on the flock; different panes dispatch in parallel.

A long-running C++ serializer with its own log cursor remains a
future option if shell+flock proves too coarse — e.g., if we want
shared driver state across dispatches, in-process retry queues, or
pane-state caching. For the MVP the shell wrapper is enough.

Verify-after-write is deferred to a follow-up. The next dispatch's
pre-flight READY check effectively verifies the previous one
indirectly: if a write didn't take, pane-state on the next attempt
will report buffer non-empty.

**Phase 3 — Pane-as-agent extensibility (when a second driver shows up).**
- Driver registry keyed by `CLAUDE_BUS_PANE_DRIVER` env var.
- Second driver (bash? headless `claude -p`?) ships with a real use
  case; designing the registry against one driver is YAGNI.

Phase 1 is committable without Phase 2. Phase 2 is committable
without Phase 3. Each phase is end-to-end testable: spawn an agent,
send a record of each type, verify the recipient receives it the way
the flags say it should.

## Why this answers the "send keys isn't enough" worry directly

"Send keys" is a single operation: write bytes, hope they land. The
new dispatch is a contract:

> *Deliver this body to this pane's input, in a state where the input
> will accept it, after dismissing anything that would block it, and
> verify the pane reflected the input before claiming success.*

The contract is encoded in the serializer's state machine. The driver
specializes the contract per program. The flags on the record tell the
transport *which* contract to invoke and *under what timing rules*. A
caller doesn't pick "send keys" or "send keys reliably" — they pick
the verb that names what they want (`bus tui` for slash, `mailbox
send` for text), and the transport delivers under that verb's
contract.

The "pane is the agent" framing makes this generalize: whatever
program lives in a pane, the serializer drives it through the same
contract via the program's driver. claude TUI today, anything else
tomorrow.

## Sources

- [Saltzer/Reed/Clark — End-to-End Arguments in System Design](https://web.mit.edu/saltzer/www/publications/endtoend/endtoend.pdf)
- [AMQP messages: properties and headers (LavinMQ)](https://lavinmq.com/documentation/amqp-messages)
- [CloudEvents Kafka protocol binding](https://github.com/cloudevents/spec/blob/main/cloudevents/bindings/kafka-protocol-binding.md)
- [ØMQ — sockets and patterns](https://zguide.zeromq.org/docs/chapter2/)
- [Pexpect — controlling interactive programs](https://pexpect.readthedocs.io/en/stable/)
- [5 serial automation gotchas — The Good Penguin](https://www.thegoodpenguin.co.uk/blog/5-serial-automation-gotchas/)
- [LMAX Disruptor — single-writer principle](https://lmax-exchange.github.io/disruptor/disruptor.html)
- [Kafka consumer groups — per-consumer offsets](https://kafka.apache.org/documentation/#intro_consumers)
- [Erlang gen_server — call/cast](https://www.erlang.org/doc/system/gen_server_concepts.html)
- [POSIX message queues in Linux — priority semantics](https://www.softprayog.in/programming/interprocess-communication-using-posix-message-queues-in-linux)
- [RabbitMQ priority queues — bucket-per-priority tradeoffs](https://www.rabbitmq.com/docs/priority)
- [Erlang selective receive — the priority anti-pattern](https://learnyousomeerlang.com/more-on-multiprocessing)
- [Inngest — queues aren't the right abstraction](https://www.inngest.com/blog/queues-are-no-longer-the-right-abstraction)
- [claude-code #23513 — tmux send-keys shell init race](https://github.com/anthropics/claude-code/issues/23513)
- [claude-code #33987 — configurable delay for send-keys](https://github.com/anthropics/claude-code/issues/33987)
- [claude-code #42391 — agent spawning command truncation](https://github.com/anthropics/claude-code/issues/42391)
- In-tree: [docs/human-agent-interaction.md](./human-agent-interaction.md);
  [docs/design-philosophies.md](./design-philosophies.md); memory
  `project_slash_via_mail` (the slash-via-mail decision this design supersedes)
