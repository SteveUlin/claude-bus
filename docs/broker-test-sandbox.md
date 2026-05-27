# Sandboxed Broker Testing: Options & Recommendation

## Headline

**Don't pick one environment — build a tower of three, sized to the
broker's three testable surfaces.** Most broker bugs live in pure
state-machine logic (cursors, retries, ACK flow) and need only a
subprocess broker against a `mktemp`'d state dir; the dispatch leg
needs real PTYs but not a real `claude`; the full end-to-end story
needs a hermetic VM only at release-gate cadence. LXC, Docker, and
nspawn solve a problem we don't have (multi-host isolation) at a cost
we shouldn't pay (image lifecycle, root). The first concrete step is
not a sandbox at all — it's fixing the half-dozen hard-coded
`/tmp/claude-bus` paths so `CLAUDE_BUS_STATE` actually isolates state.

## What the broker is, for testing purposes

Three surfaces, each with a different fidelity demand:

1. **Pure state-machine** — topic registry mutations, log append, cursor
   advancement, in-flight TTL, retry counters, audit/`inbox-human`
   escalation. No sockets, no PTYs. A pile of pure functions over the
   on-disk record format. ~80% of historical bug surface.
2. **Daemon over its socket** — JSON-RPC on `$STATE/broker.sock`, the
   250 ms delivery loop, `events.jsonl` consumption for ACKs,
   presence-sentinel and `/clear` deferral. Real broker process, but
   the "write to pane" leg is stubbed or absent.
3. **Dispatch into a real TTY** — `bus pane-send`/`bus send`, the
   `pane-state` READY check (zellij mode + scroll + modal queries),
   the dispatch state machine's normalize/retry. Needs real PTYs and,
   for the queries to mean anything, real zellij.

A monolithic answer ("LXC", "nixosTest") ignores that these three want
different setup costs and feedback loops.

## The hard-coded-path problem

`src/broker.cpp:391`, `src/delivery.cpp:{229,386,471}`,
`src/sub/sub_lifecycle.cpp:126–136`, `src/sub/sub_events.cpp:{33,103}`,
and `settings/hooks/log-event.sh:10` all reference `/tmp/claude-bus`
directly instead of `$CLAUDE_BUS_STATE`. Today, two brokers on the
same host with different `CLAUDE_BUS_STATE` dirs would still share
`events.jsonl` and `broker.log`. This forces the isolation question:
do we **fix the paths** (cheap; ~30 lines across 5 files), or do we
**isolate /tmp itself** (a container per test)?

The right answer is fix the paths. Container-per-test is the heavy
hammer this codebase doesn't need — once paths respect the env, plain
subprocesses with `mktemp -d` give you everything the container would,
at 1/100 the per-run cost.

## Options

### A. Temp state dir + subprocess broker, no zellij

```sh
export CLAUDE_BUS_STATE=$(mktemp -d)
bin/bus broker run &
broker_pid=$!
trap 'kill "$broker_pid" 2>/dev/null; wait "$broker_pid" 2>/dev/null; rm -rf "$CLAUDE_BUS_STATE"' EXIT
# … exercise via bin/bus enqueue / fetch / topic show
# … inject ACKs by appending to $CLAUDE_BUS_STATE/events.jsonl
```

- **Fidelity for state-machine bugs:** maximal. Real wire format,
  real socket, real polling loop.
- **Speed:** sub-second per case.
- **Isolation:** clean once hard-coded paths are gone.
- **Dispatch leg:** stubbed — either point recipients at a non-
  existent pane and assert the broker retries/escalates, or build a
  trivial fake `bus pane-send` shim that records bytes to a file.

### B. Separate zellij session + bash-stub agents

Add `layouts/test-stub.kdl` whose panes are titled `alice`/`bob`/… but
run a bash "stub agent" script instead of `claude`. The stub:
- echoes received text to a per-agent transcript file,
- emits `UserPromptSubmit` records into `$CLAUDE_BUS_STATE/events.jsonl`
  to ACK,
- handles `[bus-attach]`/`[bus-detach]` sentinels to test presence
  gating,
- can be told via env to simulate `/clear` (emit a blocking-op event
  then a delayed `Stop`).

Run with `ZELLIJ_SESSION_NAME=test-$$` and a per-session
`CLAUDE_BUS_STATE`. Tear down with `zellij kill-session test-$$`.

- **Fidelity for dispatch:** matches production — real PTYs, real
  zellij `query-pane`/`list-panes`, real pane-state machine.
- **Speed:** seconds per case (zellij startup dominates).
- **No LLM cost or network dependency.**
- **What it doesn't cover:** real `claude` quirks (scroll behaviour
  under streaming, mode toggles during tool use). Those are layer 3.

### C. NixOS VM tests (`pkgs.nixosTest`)

Declarative QEMU VM, Python test driver, hermetic by construction.
Boot a NixOS minimal + the bus + a stub `claude` (or, for release-
gate runs, a real one with a recorded fixture). Drive via
`machine.send_chars()` and `machine.wait_until_succeeds()`.

- **Fidelity:** whole-system, including socket activation timing,
  filesystem semantics, signals.
- **Speed:** 10–30 s boot. Wrong for inner-loop iteration; right for
  CI release gates.
- **Multi-machine:** `nixosTest` can spin up several VMs and have them
  talk, useful if we ever want to test a network-transparent broker.

### D. systemd-nspawn per test

Ephemeral container per run, bind-mounted `/nix/store` and the repo.
Real isolation of `/tmp`, no hard-coded-path refactor required.

- **Fidelity:** real OS, real zellij, real PTYs.
- **Speed:** ~1 s boot.
- **Setup tax:** needs root or polkit; requires a bind-mount recipe
  that survives NixOS rebuilds.
- **The case for it:** an escape hatch *if* the hard-coded-path fix
  turns out to be load-bearing in surprising ways. Otherwise option A
  subsumes it.

### E. LXC (sulin's starting idea)

Long-lived system containers (LXD or `nixos-container`).

- **Strength:** persistent multi-tenant system simulation. Networking
  namespace. Several "hosts" with their own state.
- **Mismatch:** the broker is not network-aware; we don't need
  multi-host scenarios; tests want *ephemeral* environments, not
  persistent system containers. Image and init cost is amortised over
  many tests, but each test still wants a clean slate inside the
  container, which means another isolation layer underneath.
- **Verdict:** the right tool for "spin up a tiny lab to dogfood
  multi-user bus behaviour by hand", **not** for "give me 200 tests
  per minute against a clean broker". Wrong axis.

### F. Docker

Works. No advantage over nspawn on a NixOS host; adds an image-build
cycle the local-iteration story doesn't want. If we ever ship a CI
image to a non-NixOS runner, revisit.

### G. PTY-stub harness (no zellij)

Allocate `posix_openpt` pseudoterminals and register them with the
broker via a test-only shim in `src/pane.cpp` that takes a fake pane
backend. Real terminal escape semantics; no zellij dependency.

- **Strength:** lets us assert byte-exact dispatch output in a unit
  test, very fast.
- **Cost:** requires factoring `pane.cpp` so its pane-discovery and
  state-query functions are injectable. Modest refactor, ~1 day.
- **Verdict:** good follow-on to option A once we want byte-level
  dispatch coverage without paying the zellij startup tax of option
  B. Not the first thing to build.

## Recommendation

Build the tower in this order, stopping at each layer until a real
gap forces the next:

1. **Now: option A, after the path fix.** Land the 30-line refactor
   that makes `events.jsonl`, `broker.log`, and the agents registry
   respect `CLAUDE_BUS_STATE`. Extend `tests/bus-itest.sh` (or add
   `tests/bus-broker-test.sh`) with the `mktemp` + background-broker
   pattern. Use it to cover cursor advancement, retry/escalation, the
   ACK-on-`UserPromptSubmit` flow, blackboard fast-forward, pubsub
   cascade, and presence-sentinel deferral.

2. **Next: option B, gated on a real layer-1 gap.** Add a
   `layouts/test-stub.kdl` + a bash stub-agent when a class of bug
   shows up that A can't see — first candidate is `/clear` deferral
   under real Stop events.

3. **Eventually: option F nixosTest.** Wire it once we want a release
   gate; tag and run pre-cut, not per-PR.

Skip C/D/E/F unless one of the layers above hits a wall that
isolation specifically would unstick. The hard-coded-path fix
removes the most common reason someone would reach for a container.

## What sulin's LXC instinct was getting right

The instinct correctly identified that *shared `/tmp`* is the
load-bearing isolation gap today. LXC just over-solves it — a
namespace-per-test or a fixed code-path is enough. If we keep hitting
"two test runs trampled each other's state because path X was
hard-coded", option D (nspawn) or option F (NixOS VM) is the right
upgrade, not LXC.

## Open questions for sulin

1. Is there appetite for the hard-coded-path refactor as a
   prerequisite, or should we add an option-D nspawn harness as a
   shorter path?
2. Do we want the stub-agent to live in `tests/` (bash) or in `src/`
   as a C++ binary (faster, can share the bus client lib)?
3. Should test infrastructure target `tests/bus-itest.sh` (extending
   what exists) or a new `tests/broker/` tree with per-scenario
   shell files?
