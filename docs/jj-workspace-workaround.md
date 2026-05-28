# jj workspaces + git worktrees — workaround for per-agent isolation

Author: elodin · For: auri / sulin · Status: research only, no code yet.

## The scoop-bundle problem

Three times this session, my `jj describe` ended up bundling another agent's WIP files because we share a single working copy on disk (`/home/sulin/claude-bus`). bast's ideation flagged this as the "scoop-bundle" pattern; attribution via `Co-Authored-By: bast` is the diagnostic, but the *preventative* fix is per-agent worktrees — each agent operates on its own working copy, the shared object store collects everyone's commits, and `jj` operations stop cross-pollinating WIPs.

The natural jj answer is "one jj workspace per agent, all backed by the same colocated git/jj repo." Today's jj doesn't support that. This doc explains why and what we can do anyway.

## Tracking issue + constraint

The relevant tracking issue is [jj-vcs/jj#8052 "Tracking issue for colocated workspaces"](https://github.com/jj-vcs/jj/issues/8052), opened 2025-11-17 and still ONGOING. Its step (1) — "Finalise and merge Colocated workspaces: internal support [#8252]" — is in progress, and most subsequent steps (workspace-detection corner cases, `jj git colocate enable/disable` workspace support, `git_head()` per-workspace) are TODO. The proof-of-concept work happened in [PR #4588 "Colocated workspaces using git worktrees"](https://github.com/martinvonz/jj/pull/4588), which was closed once the author split the change into smaller PRs ([#4644](https://github.com/jj-vcs/jj/pull/4644) onward).

The constraint as it stands today:

- Only **one** workspace per repo can be colocated with git (own a `.git` and accept native `git` commands).
- `jj workspace add` creates additional jj workspaces, but they're **pure-jj**: no `.git` in the new workspace directory, so `git status` / `git log` / `git diff` don't work there. `jj` operations work; `jj git push` works (uses the shared backend, not the workspace's `.git`).
- `jj git init --colocate` will refuse to run inside an existing git worktree — that's step (3) of the tracking issue, explicitly called out as TODO.

So the elegant "git worktree + colocated jj workspace per agent" shape isn't available until #8052 ships. We need a workaround that lives within today's constraints.

## Workarounds, ranked

### 1. Per-agent pure-jj workspaces backed by the main colocated repo *(recommended)*

From the main repo, `jj workspace add $BUS_ROOT/.workspaces/<agent>` for each agent. Agents `cd` to their workspace before claude starts. Each workspace has its own working copy on disk; they share `.jj/repo` (commits, branches, op log).

- **Native git in agent workspaces**: doesn't work. No `.git` directory there.
- **jj in agent workspaces**: works fully. Includes `jj describe`, `jj new`, `jj diff`, `jj log`, `jj git push`, `jj git fetch`.
- **Cross-agent visibility**: shared op log means agent A sees agent B's commits via `jj log`; the shared backend means `jj git push` from any workspace operates on the same repo.
- **Concurrency**: jj's per-op locking is fine for the typical "agents touch different files" case; same-file races produce normal jj conflicts.
- **Disk cost**: one working copy per agent. For claude-bus today (~60 MB checked-out source), 4-5 agents is ~300 MB extra. Not free, not concerning.

This is what jj's `jj workspace` is *designed for*. The "colocated workspaces" feature is a convenience layer on top — without it, every workspace works, just not all of them get a `.git`.

### 2. Git worktree per agent, ignore jj outside the main workspace

Each agent's directory is a `git worktree add` of the main repo. Agents use *git*, not jj. `git commit`, `git push` per agent.

- **Pro**: agents get full git tooling.
- **Con**: bus's whole code review / commit message style is jj-based (commit descriptions, op-log workflow). Switching agents to git breaks the existing review flow. Plus, the project's CLAUDE.md is explicit about jj convention.
- **Verdict**: technically works, but a regression on the dev workflow. Skip.

### 3. Separate clones per agent

`git clone` once per agent into a distinct directory. Each is its own repository with its own object store.

- **Pro**: maximum isolation. No shared state surprises.
- **Con**: huge disk cost (full object store × N). Cross-agent commit visibility requires pushing through `origin` and fetching — slow and human-mediated. Manual coordination on rebases.
- **Verdict**: only worth it for projects where agents work on genuinely independent branches. Overkill for the bus's tight-coupling pattern.

### 4. Status quo + commit attribution (bast's #1 ideation pick)

Don't isolate; just add `Co-Authored-By:` to commits so the scoop-bundle is *diagnosable*. Doesn't prevent the bug — agents still step on each other's WIPs — but mismatched authorship in a single commit stands out in `jj log`.

- **Pro**: ~10 LOC. No filesystem layout change.
- **Verdict**: complementary to (1), not a replacement. Ship both — attribution helps spot residual bugs even after isolation lands.

## Recommendation — (1) per-agent pure-jj workspaces

The cleanest path forward today: every agent gets its own `jj workspace add`'d directory under `$BUS_ROOT/.workspaces/<agent>`. Agents work there. The main `$BUS_ROOT` stays as the colocated workspace sulin uses directly; that's where native git commands still work for human-side operations (`gh pr create`, `git rebase --interactive` if anyone wants it, etc.).

Why this over waiting for #8052:

- The tracking issue is open with no ETA; the proof-of-concept PR was closed and re-split. Realistically a quarter+ before colocated-workspaces lands fully.
- The constraint we're working around — "agent workspaces lose native git" — costs almost nothing in practice. All claude-bus agent activity goes through jj (commit description, push, fetch). Hooks don't shell out to git. The narrow exception is `gh pr create` (which sulin runs from the main workspace anyway).
- The fix is forward-compatible: when #8052 lands, we can opt agent workspaces into colocation with `jj workspace add --colocate` and everything else stays the same.

## What'd change in setup

- **`bin/agent-launch`** (bast's territory): on agent boot, ensure `$BUS_ROOT/.workspaces/$CLAUDE_BUS_AGENT_ID` exists. If not, `jj workspace add` it from `$BUS_ROOT`. Then `cd` to the workspace before `exec claude`.
- **`.gitignore`** (or its jj equivalent): exclude `.workspaces/` from the main repo's tracking.
- **CLAUDE.md** / per-agent role files: note the new convention. Agents `pwd` in `$BUS_ROOT/.workspaces/<agent>`; their `jj describe`, `jj new`, `jj git push` operate there.
- **Hooks** under `settings/hooks/`: audit for any that assume agent CWD == `$BUS_ROOT`. The `agent-register.sh` and `log-event.sh` paths take `payload.cwd` from claude's hook payload, which would correctly reflect the workspace dir — no change needed.
- **CI / build** (`cmake`): builds happen in the main workspace, output binary at `$BUS_ROOT/bin/bus`. Agents read that binary directly — no per-workspace rebuild.
- **Composability with bast's commit-attribution ideation pick**: layer on top. Pure-jj workspace isolation removes the scoop-bundle bug at the source; `Co-Authored-By` makes residual cases (e.g., if an agent reaches into another's workspace) visible.

## Open questions for sulin

- The `.workspaces/` directory: under `$BUS_ROOT` (visible in `jj log` activity but ignored from tracking) or under `/tmp/claude-bus/workspaces/` (ephemeral, gone on host reboot)? My lean: under `$BUS_ROOT`, gitignored. Persistent so an agent's working copy survives across host reboots; visible enough to debug.
- Should `bin/agent-launch` auto-create the workspace if it doesn't exist, or require explicit `bus workspace init <agent>` first? My lean: auto-create on first launch. Fewer manual steps; the cost (an empty workspace directory) is negligible if the agent never actually runs.
- Should agent workspaces share a single op log with the main, or get distinct heads to prevent crosstalk? jj's default is shared — every agent sees every other agent's commits via `jj log`. That's probably what we want; the op log IS the cross-agent visibility surface.

Sources:
- [jj-vcs/jj#8052 Tracking issue for colocated workspaces](https://github.com/jj-vcs/jj/issues/8052)
- [martinvonz/jj#4588 Colocated workspaces using git worktrees (closed)](https://github.com/martinvonz/jj/pull/4588)
- [jj-vcs/jj#4644 follow-up PR](https://github.com/jj-vcs/jj/pull/4644)
- [Jujutsu docs — Git compatibility](https://docs.jj-vcs.dev/latest/git-compatibility/)
- [Introducing jjw: a workspace manager for jj](https://aran.dev/posts/introducing-jjw-jj-workspace-manager/)
