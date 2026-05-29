# The broker — async queued delivery (full spec)

Reference for the broker daemon. CLAUDE.md carries only the every-turn gist;
this file is the full specification, read on demand.

`bus msg send` writes into a pane's TUI buffer and submits immediately —
useful for urgent direct messages but contends with the human keyboard
and can interleave with other senders. For queued / async / typed
delivery, go through the **broker daemon**.

The broker (`bus broker run`) is the single source of truth for the
bus. It owns:
- The topic registry (`$STATE/topics.json` — what topics exist + their
  kinds + per-kind config).
- Topic logs (`$STATE/topics/<name>.log` — append-only records, v4
  wire format).
- Per-(topic, consumer) cursors (`$STATE/cursors/<topic>/<consumer>.cursor`).
- The delivery loop (every 250 ms, scans topics, dispatches matching
  records to recipient panes via a flock'd TTY write).
- In-flight tracker (`$STATE/in-flight/<msg_id>.json`), retry timers,
  and audit / `inbox-human` escalation on exhausted retries.

Hooks now ONLY emit state events to `events.jsonl` (via
`settings/hooks/log-event.sh`). They never pull mail or dispatch
slashes — the broker decides when to push.

**Launch contract.** The broker must be launched as a direct child of
zellij — `layouts/fleet.kdl`'s floating pane is the canonical path,
and `prctl(PR_SET_PDEATHSIG, SIGTERM)` ties the broker's lifetime to
that parent so closing the pane / restarting zellij brings it down
cleanly. Do **NOT** use `nohup`, `setsid`, or `disown` to background
it from a tool-call shell or anywhere else: any of those defeat the
parent-death signal and leave the broker reparented to init,
surviving zellij restarts as an orphan. To restart manually, prefer
`zellij action new-pane --floating -- /path/to/bus broker run`
or stop-via-`bus broker stop` + relaunch the layout. See
`docs/broker-lifetime-fix.md` for the diagnosis.

**Topic kinds:**

| Kind | What it does |
|---|---|
| `agent-inbox` | Single recipient. Broker pushes records into the recipient's pane (inline if body ≤ 1024 bytes; pointer + payload file if larger). Auto-created as `inbox-<name>`. |
| `tui-commands` | Single recipient. Broker pushes via the dispatch state machine (`pane-state` READY check, normalize, retry). `/clear` and `/compact` mark the agent as having a blocking-op; subsequent delivery defers until the next `Stop` event. Auto-created as `commands-<name>`. |
| `work-queue` | Multi-consumer pull. Producers `bus msg enqueue`; consumers `bus msg fetch` (each fetch advances the cursor). |
| `pubsub` | Declared subscribers. On enqueue, broker cascades the record into each subscriber's `inbox-<name>` (canonical record stays on the pubsub topic for audit / replay). |
| `blackboard` | Last-value-wins. New writes fast-forward the cursor; readers `fetch` non-destructively. |
| `append-log` | Write-only audit. The broker uses `audit` itself for delivery-failure records. |

**The `[bus-attach]` sentinel** still controls presence: the broker
defers ALL records for an agent while `$STATE/presence/<agent>` is
fresh. The cursor advances in FIFO order with no per-record bypass.

**Cursor semantics by kind:**

- `agent-inbox` / `tui-commands`: cursor advances on ACK, not on
  dispatch. ACK comes from `events.jsonl`: `UserPromptSubmit` acks
  the oldest pending inbox record for that agent; `Stop` acks a
  blocking-op slash. Until ACK, the record sits at the head and the
  in-flight gate prevents re-dispatch.
- `work-queue`: cursor advances on `bus msg fetch` (pull). Multiple
  consumers each get distinct records.
- `blackboard`: cursor stays at the latest record; `fetch` is
  non-destructive.
- `pubsub`: per-subscriber cursors on each inbox-<sub>; the original
  pubsub log keeps everything.

**Reliability:** every dispatch creates an in-flight file. If no ACK
arrives within `$CLAUDE_BUS_ACK_TIMEOUT_MS` (default 60 s), the broker
retries up to 3 attempts, then escalates: appends a record to the
`audit` topic and mails `inbox-human` with the failure summary, then
advances the cursor past the failed record so the queue can drain.

Mailbox state lives at `/tmp/claude-bus/mailbox/<name>.log` (one binary log file per recipient). Single-consumer per mailbox; messages capped ~4 KiB.
