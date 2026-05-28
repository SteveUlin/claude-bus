# Broker-orphan diagnosis + lifetime-fix proposal

Author: elodin · For: comms / sulin · Status: proposal, no code yet.

## What went wrong

The broker is supposed to die with its launcher pane: `CLAUDE.md` says "broker's lifetime is tied to this pane: closing it terminates the broker." The fleet layout (`layouts/fleet.kdl`) backs this up — `bus broker run` runs as the floating pane's `bash -c "exec …"`, so the broker process IS that pane's command, and zellij killing the pane sends SIGHUP to the broker.

That contract held when launches went through the floating pane. It broke when I restarted the broker during the auto-clear ship (commit `cad6f8e`). I used:

```
nohup /home/sulin/claude-bus/bin/bus broker run >> /tmp/claude-bus/broker.log 2>&1 &
disown
```

That launches the broker as a background child of my bash tool-call shell. When the shell exited (at tool-call end), the kernel reparented the broker to PID 1 (init). It survived the subsequent zellij restart as an orphan with PPID=1 — exactly the pattern comms found on PID 3334098. comms killed it manually, then respawned a fresh broker through the floating pane (PID 3448617, PPID=zellij). Healthy chain restored.

I caused the same orphan-class earlier in this session at least twice (when I SIGKILL'd the wedged broker and restarted with `nohup`). Each `nohup ... &` from a bash tool-call invocation is a fresh orphan candidate.

Why `nohup` makes it worse: nohup ignores SIGHUP. When the bash shell that backgrounded the broker terminates, the kernel sends SIGHUP to its children. `nohup` swallows it — but the broker has no PR_SET_PDEATHSIG either, so nothing else triggers shutdown. Broker survives indefinitely, parented to init.

## The four candidate fixes

### (1) Process group dies with launcher (`setpgid`)

Put the broker into a process group rooted at the launcher pane. When the pane closes, the kernel sends SIGHUP to the whole group.

- **Cost:** ~10 LOC.
- **Robustness:** Defeated by `setsid` (breaks process group association) and arguably by `nohup ... &` (the broker becomes its own group when `&` runs). Doesn't help for the exact mistake I made.

### (2) `getppid()` check at startup, refuse if PPID != zellij

Walk `/proc/<ppid>/comm` at boot, refuse to start if the parent isn't zellij.

- **Cost:** ~20 LOC.
- **Robustness:** Detects the orphan AT START but won't help when the broker is *already running* and the launcher dies mid-flight. Also brittle — the floating-pane chain is `zellij → broker` (no bash intermediary, because zellij runs the command directly), but the layout uses `bash -c "exec bin/bus broker run"`, which does keep PPID=zellij thanks to the `exec`. Any layout change that drops `exec` breaks this. False-rejects on `bus up` / nohup-from-script / systemd legitimate cases.

### (3) Restart-script that always uses the floating pane

Tooling: add `bus broker restart` (or extend `bus broker stop` + the restart side) to do `zellij action new-pane --floating --command "..."`. The right path becomes the obvious path.

- **Cost:** ~30 LOC.
- **Robustness:** Pure procedure. Doesn't *prevent* the ad-hoc `bus broker run &` mistake; it just removes the temptation. Still useful as a sibling to the kernel-level fix below.

### (4) `prctl(PR_SET_PDEATHSIG, SIGTERM)` at startup

Linux-specific syscall. Tells the kernel: when MY parent process dies, send ME this signal. Set once at the very start of `runBroker`, after the singleton-flock and before any RPC work.

- **Cost:** ~10 LOC (one `prctl` call + an `<sys/prctl.h>` include + a logEvent for the failure path).
- **Robustness:** Fires when the immediate parent exits — including the bash-from-tool-call case that orphaned my restart. The broker's existing SIGTERM handler triggers gStopFlag → graceful shutdown. Immune to `nohup` (PR_SET_PDEATHSIG is a separate signal from SIGHUP). The one case it can't catch: launches with `setsid` (broker becomes its own session leader, reparent is to init immediately, PR_SET_PDEATHSIG fires against init which never dies). But `setsid` is a deliberate detach and we don't use it.

## Recommendation — ship (4), document (3) as the right launch path

**Code change: `prctl(PR_SET_PDEATHSIG, SIGTERM)` in `src/broker.cpp::runBroker`, immediately after the flock-singleton block.** That's the cheapest fix that catches the actual mistake comms surfaced (a `nohup ... &` from a tool-call shell). My SIGTERM handler is already wired (`installInterruptHandlers(onStop)` in `rpc::Server::run`); it sets `gStopFlag` and the pselect loop exits cleanly.

Concrete shape:

```cpp
#include <sys/prctl.h>
…
// After flock acquired, before topic-registry load:
#ifdef PR_SET_PDEATHSIG
if (::prctl(PR_SET_PDEATHSIG, SIGTERM) < 0) {
  logEvent(cfg.state_dir, "WARN",
           std::format("prctl PR_SET_PDEATHSIG failed: {}",
                       std::strerror(errno)));
  // Not fatal — broker still runs, it just won't auto-die with
  // its parent. Better visible warning than silent.
}
#endif
```

The `#ifdef` guards macOS / BSD; the bus already targets Linux so this is belt-and-suspenders. Total surface: ~8 LOC. Well under the 50-LOC ceiling.

**Documentation alongside: launch contract.** Add a short section to `CLAUDE.md` (or a new `docs/broker-launch.md`) saying:

> The broker must be launched as a direct child of zellij — `layouts/fleet.kdl`'s floating pane is the canonical path. Do NOT use `nohup`, `setsid`, or `disown` to background it: any of those defeat the parent-death signal and orphan the broker. To restart manually, prefer `zellij action new-pane --floating --command "$BUS_ROOT/bin/bus broker run"`.

That's the procedural belt to go with the kernel-level suspenders.

## What I'm NOT proposing

- **No PPID check at start.** PR_SET_PDEATHSIG covers the same cases without the brittleness of "what counts as the right parent."
- **No setpgid.** Doesn't help with `&`-backgrounding mistakes; redundant with PR_SET_PDEATHSIG.
- **No `bus broker restart` subcommand yet.** Worth adding eventually — `bus broker stop` already exists, the restart half would be a 10-line wrapper that does `stop && zellij action new-pane --floating …`. Different commit; not blocking.

## Ready to ship when approved

Code change is ~8 LOC in `src/broker.cpp`. Needs a broker restart to take effect (the running broker is the unfixed binary). Sequence per the auto-clear pattern: SIGKILL old broker, launch new via floating pane (via zellij action), confirm new PID's parent IS zellij, confirm `bus broker status` is alive. Holding until comms acks.
