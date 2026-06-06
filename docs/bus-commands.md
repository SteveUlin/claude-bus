# `bus` command catalog

The full subcommand reference. `bus help` is the canonical, always-current
list; this file is the annotated catalog. CLAUDE.md links here rather than
reproducing it, so the catalog isn't billed on every agent turn.

Everything runs through the unified `bin/bus` binary. The broker daemon
(`bus broker run`) owns all topic state and drives delivery; CLI tools talk to
it via JSON-RPC on `$STATE/broker.sock`.

Lifecycle:
- `bus broker run` — the only broker verb. Foreground process bound to
  its pane's lifetime. Launched by `layouts/fleet.kdl` as a floating
  pane in the ops tab; closing the pane terminates the broker. A
  flock on `$STATE/broker.pid` keeps it a singleton — a second
  `bus broker run` exits 1.
- `bus spawn NAME` — open a new zellij tab for an agent (still uses
  `bin/agent-launch` shell for session-UUID discovery + claude exec).

Topic registry:
- `bus topic create NAME --kind KIND [--subscribers a,b,c]
  [--retention-ms N]` — declare a topic. Known kinds:
  `agent-inbox` / `tui-commands` / `work-queue` / `pubsub` /
  `blackboard` / `append-log`.
- `bus topic list` / `bus topic show NAME`.
- `inbox-<X>` / `commands-<X>` auto-create from name pattern on first
  enqueue.

Produce / consume:
- `bus msg enqueue TOPIC body [--protocol] [--deliver-when] [--ttl]`.
- `bus msg mail AGENT body` — sugar: enqueue → inbox-AGENT.
- `bus msg slash AGENT /command` — sugar: enqueue → commands-AGENT with
  deliver_when=idle.
- `bus msg broadcast TAG body --to AGENTS` — sender-side fan-out to each
  agent's inbox.
- `bus msg fetch TOPIC [--consumer ID]` — pop (non-destructive on
  blackboard).
- `bus msg peek TOPIC [--consumer ID] [--limit N]`.
- `bus msg body MSG_ID` — side-effect-free read by id.

Direct + low-level (bypass broker):
- `bus msg send NAME TEXT` — type TEXT + Enter into the named pane,
  holding the per-pane TTY flock (single-writer guarantee).
- `bus pane-send PANE_ID TEXT` — raw write to a pane id, no flock.
  Used internally by the broker delivery + dispatch state machine.
- `bus pane-id NAME` / `bus pane-state NAME`.

Viewers + introspection:
- `bus monitor` — colored dashboard, 1-Hz refresh.
- `bus topic inbox NAME` — tail-follow an agent's `inbox-NAME` topic.
- `bus agent-bar NAME` — per-tab status strip.
- `bus events [--since TS] [--agent NAME]` — events.jsonl tail.
- `bus state [AGENT]` — broker's view of agent lifecycle.
- `bus inflight` — in-flight deliveries (debug).

`bus version` / `bus help`.
