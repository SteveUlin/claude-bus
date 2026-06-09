# Per-project agents — launching an agent in another repo

An agent doesn't have to work in `claude-bus`. `agent-launch` splits two roots:

- **BUS_ROOT** — `claude-bus` itself: the `bus` binary, the hooks, the roles,
  and `$STATE`. An agent is always a citizen of *this* bus.
- **PROJECT_ROOT** — the repo the agent actually *works* in: it gets its own jj
  workspace + cwd there. Defaults to BUS_ROOT (so a plain agent works in
  claude-bus, unchanged).

`--project-dir DIR` points an agent at a second repo. It still talks to the same
broker, runs the same shared hooks, and writes the same `$STATE` — it just edits
a different tree. This is how the taro work runs (agents spawned into `~/taro`).

## Dynamic — `bus spawn` (the common path)

```sh
bus spawn --project-dir ~/taro --role taro-spoke --profile worker taro-1
```

That creates a zellij tab `taro-1`, a jj workspace + cwd under `~/taro`, and
launches `claude` there with the `taro-spoke` role injected. The agent appears
on the bus immediately — mail it, watch it in `bus monitor`, etc.

- **Persisted across relaunch.** `bus spawn` records the peer (name + role +
  `project_dir` + profile) in the dynamic-peer registry, so `bus restore-peers`
  (run at fleet startup) re-spawns it into the same repo. `profile` MUST persist
  or a restore silently demotes an orchestrator to the worker envelope.
- **Tear down** with `bus despawn taro-1` (prunes the registry entry + closes
  the tab + reaps its broker topics).
- Session continuity is free: the re-spawn resumes the agent's prior session
  (`--continue`), keyed by name + workspace.

Spawn several into one project and they share that repo's broker + a work queue
(see [work-queue-dispatch.md](work-queue-dispatch.md)) — e.g. `bus queue push
taro-builds "…"` fanned out to the taro spokes.

## Static — a layout that launches agents in a project

A layout pane launches an agent by `exec`-ing `agent-launch` with the same
flags. Model it on `layouts/fleet.kdl`'s respawn-loop wrapper, adding
`--project-dir`. Parameterize the project with an env var so one layout serves
any repo:

```kdl
// launch with:  CLAUDE_BUS_PROJECT_DIR=~/taro zellij --layout layouts/project.kdl
pane command="bash" {
    args "-c" "B=${CLAUDE_BUS_ROOT:-$PWD}; cd \"$B\"; \
        P=${CLAUDE_BUS_PROJECT_DIR:?set CLAUDE_BUS_PROJECT_DIR to the repo}; \
        exec \"$B/bin/agent-launch\" --project-dir \"$P\" --role taro-spoke taro-1"
}
```

The agent-launch flag surface: `agent-launch [--role ROLE] [--project-dir DIR]
[--profile worker|research|orchestrator] NAME`.

## Relation to docs/external-projects.md

`external-projects.md` proposes a larger abstraction — *sessions* (a named fleet
pointed at a project), per-agent *kinds*, and a layout generator. That's the
fuller vision and is mostly unbuilt. `--project-dir` is the minimal slice that
shipped: per-agent project targeting, no new session/kind machinery. Reach for
the bigger design only if hardcoding spawns/layout panes per project starts to
hurt.
