# Orchestrator launch profile — web-capable, gate-up, write-gated

**Status:** DESIGN (spec before build; auri reviews before land).
**Owner:** bast (launch profiles / `bin/agent-launch` / config materialize).
**Relates to:** [research-isolation.md](research-isolation.md) (the `worker`/`research`
profiles + netns cage this extends), the config-materialize model in `CLAUDE.md`.

## 1. Principle

**Internet-exposure and bypass are mutually exclusive.** An agent that can fetch
web content (`WebSearch`/`WebFetch`) must run **gate-up** (`--permission-mode
default`, auto), never `--dangerously-skip-permissions` — so injected web content
can never silently trigger an action. The human-answered prompt *is* the safety
mechanism for the dangerous post-web step (a write). This is not merely policy:
under bypass, fan-out subagents **inherit** the permissive mode and can't be
restricted (§3.1), so bypass *structurally* defeats the read-only-subagent design —
**non-bypass is required by the mechanism**, the strongest argument for the profile.

This profile is the web-capable **orchestrator**: auto mode, web + write tools,
where **writes prompt the in-pane human**. It fans out read-only headless
subagents that do the web reading; the orchestrator alone holds the answerable
write gate.

This profile does **not** depend on the SEC-1 nixos module — the auto-gate is the
primary defense, SEC-1 is now an optional defense-in-depth layer.

## 2. Two layers — do not conflate

There are two distinct "agent" scopes, with two distinct permission mechanisms:

| Layer | What | Launched by | Permission source |
|---|---|---|---|
| **Orchestrator** | a bus-pane `claude` session (a fleet peer) | `agent-launch --profile orchestrator` | session launch flags (`--permission-mode`, `--allowedTools`) |
| **Subagent** | a headless in-process worker (Workflow `agent()` / Task) | the orchestrator, at runtime | **inherits** the orchestrator's auto-mode; a headless write **auto-denies** (§5) |

"Orchestrator = auto + write-gated" is the **session** layer; "subagents =
read-only" falls out of the **same** auto-mode — a headless subagent can't answer a
write prompt, so the write auto-denies. No separate subagent config: the one
non-bypass session setting gives both the gated orchestrator and the read-only
subagents (§5).

## 3. THE KEY QUESTION, answered: separation IS possible

> Can claude-code separate the orchestrator's permission set (auto + write-gated)
> from the subagents' set (read-only allowlist, no prompt)? Or is it only one
> session-wide `--allowedTools`/`--permission-mode`?

**Answer: it's AUTOMATIC under auto-mode — and holds ONLY IF THE ORCHESTRATOR IS
NON-BYPASS.** (Authoritative via claude-code-guide, routed by auri — the sanctioned
channel, no web from this bypass peer; code.claude.com sub-agents +
permission-modes.) Read-only subagents need **no separate config**:

- A headless subagent **inherits** the orchestrator's auto-mode. Any `Edit`/`Write`
  it attempts raises a permission prompt it cannot answer → the background subagent
  **AUTO-DENIES the prompt and CONTINUES** — fail-closed and graceful, it does
  **not** hang. So the write never lands: **read-only by auto-deny**, no
  `.claude/agents` toolset required (§5).
- This is *why no custom type is needed*. The earlier "hangs loud-and-stuck"
  framing is retracted (it came from stale GH issues I should not have fetched);
  the authoritative answer — auto-deny + continue — supersedes it.

### 3.1 THE CRITICAL GOTCHA — bypass-orchestrator is FORBIDDEN BY THE MECHANISM

**If the orchestrator launches with `--dangerously-skip-permissions` (bypass) or
`acceptEdits`, its SUBAGENTS INHERIT that permissive mode and CANNOT be
restricted** — the parent mode **overrides** the subagent's `permissionMode` /
`tools`-deny. So a bypass orchestrator ⇒ ungated, write-capable subagents ⇒ the
**exact hole** the security model closes (web-injected content acting silently
through a write-capable headless subagent).

Therefore the orchestrator **MUST** run `--permission-mode default` for the
read-only-subagent restriction to hold at all. This is the strongest argument for
the whole profile: **web-capable ⇒ non-bypass is not policy preference, it is a
mechanism requirement** — bypass *structurally defeats* subagent restriction.

**Also session-wide:** `settings.json` `permissions.allow`/`deny` apply uniformly —
no per-agent block — so we can't `deny: Edit` there without blocking the
orchestrator's own gated writes. Per-subagent restriction comes from the subagent
layer (§5), and only holds under a non-bypass orchestrator (§3.1).

## 4. Orchestrator profile spec (`agent-launch --profile orchestrator`)

A third `--profile` case, inverse of `research` (which denies writes; this gates
them). Non-bypass, auto mode, safe set pre-approved so the human isn't spammed,
`Edit`/`Write` left to **prompt**:

```sh
orchestrator)
    # Web-capable + write-capable, gate UP. The safe read/web/coordinate set is
    # auto-approved (no prompt); Edit/Write/NotebookEdit are in NEITHER list, so
    # they fall to --permission-mode default = PROMPT the in-pane human (the gate
    # IS the safety mechanism for the dangerous post-web action). Agent/Task/
    # Workflow auto-approved so fan-out doesn't prompt. NOT bypass, NOT acceptEdits.
    bypass_flag=()
    perm_flag=(--permission-mode default \
               --allowedTools 'Read,Grep,Glob,WebSearch,WebFetch,Bash(bus *),Agent,Task,Workflow')
    # (no --disallowedTools: Edit/Write must remain AVAILABLE, just gated)
    export CLAUDE_CODE_EFFORT_LEVEL=ultracode   # xhigh + workflow-orchestration ON
    claude_effort_flag=()                        # drop --effort so the env wins
    ;;
```

Network: the orchestrator needs web, so it is **not** caged to the
anthropic-only worker tier. Either no cage (auto-gate is the defense) or the
`research`/scholar squid allowlist tier — auri's call; default to **no netns
cage** for v1 since auto-gate is primary and SEC-1 is optional.

**Workflow orchestration ON (the `ultracode` env).** `ultracode` is the Claude
Code setting that sends `xhigh` effort **and** turns on automatic workflow
orchestration — the orchestrator profile's whole point. It is *session-only-
rejected* in `settings.json`; the **only permanent mechanism** is the env var
`CLAUDE_CODE_EFFORT_LEVEL=ultracode`, read at claude startup. We set it
**profile-tied, not name-tied** — the orchestrator profile *is* a workflow-
orchestrator by definition, so it gets workflow-on by construction every relaunch
(satisfies "only the orchestrator-profile agent gets it," and is future-proof vs
hardcoding a name). The orchestrator is never caged (`netns=""`), so it always
takes the plain `exec` and inherits the export — no `-E` forwarding needed. We
also **clear `claude_effort_flag`** (drop the name-based `--effort`): an explicit
`--effort` would **override** the env (flag > env) and pin `high`, defeating
ultracode's `xhigh`. Precedence (flag > env) is confirmed at the §7 first-launch
gate.

Semantics this relies on (gated in §7): under `--permission-mode default`,
`--allowedTools` *pre-approves* the listed tools while a tool in **neither**
`--allowedTools` nor `--disallowedTools` still **prompts**. (The `research`
profile uses both lists together, which already implies this three-way split:
allow = auto, disallow = hard-block, unlisted = prompt.)

## 5. Subagent envelope — read-only by auto-deny (no custom type)

**Subagents inherit the orchestrator auto-mode; headless ⇒ writes auto-DENY
(fail-closed), so read-only-by-auto-deny + web; no custom type.** A headless
subagent that attempts `Edit`/`Write` hits a permission prompt it cannot answer →
the background subagent auto-denies and continues (§3) → the write never lands.
Read-only is therefore guaranteed by the *mechanism*, not by a `.claude/agents`
toolset — so we ship **no custom subagent type and no materialize surface**. The
orchestrator's Workflow script picks each subagent's model per `agent()` call, so
dropping the static type costs nothing on capability.

**The one thing the orchestrator must still do: pre-approve web.** Since background
subagents auto-deny any *prompt*, if `WebSearch`/`WebFetch` would prompt they'd be
silently denied (no hang, but no fetch). So the orchestrator session pre-approves
`WebSearch`/`WebFetch` (+ `Bash(bus *)`) in its `--allowedTools` (§4); subagents
inherit that auto-approval and can actually research. (§7 confirms the propagation
at first orchestrator launch.)

## 6. Failure mode and Bash

**Failure mode (fail-closed, graceful):** a headless subagent never lands a write —
any `Edit`/`Write` it attempts prompts, and a background subagent **AUTO-DENIES and
CONTINUES** (it does **not** hang). So the failure direction is *silently-denied*,
never *silent-write* and never *wedged*. The flip side is the §5 consequence: a
needed tool that isn't pre-approved is also silently skipped — so pre-approve the
subagents' real needs (web, `Bash(bus *)`).

**Bash — steer subagents to native read tools.** Under §4 only `Bash(bus *)` is
granted, so a subagent's arbitrary `Bash` (e.g. `rg`/`find`) auto-denies and the
read silently fails. The orchestrator's prompt should steer subagents to the
granted native tools — `Read`/`Grep`/`Glob` (all auto-approved) — over `Bash` for
read-only work. A line in the orchestrator role/prompt covers it.

**Nesting:** even if a subagent spawns a further (nested) agent, the non-bypass
orchestrator means the inherited mode is still the *default* (gated) one, not
bypass — so a nested child still cannot silently write (§3.1). Treat subagents as
leaves anyway; the orchestrator is the sole fan-out point.

## 7. Pre-land empirical GATE (SEC-1-style: verify, don't trust the docs)

The mechanism is now authoritative (guide), so the gate just confirms it fires as
documented before land (SEC-1 precedent). **HONEST FINDING (bast, build time): a
bypass peer cannot actually run any of these** — check 1 needs *interactive*
prompt observation (headless auto-denies, so the prompt can't be seen), and check 2
would require spawning a web-capable subagent = web-under-bypass (the very
hole). So all three fold into the **first non-bypass orchestrator launch**. This
recursively reinforces the thesis: a bypass peer can't even safely *test* the
read-only envelope — only the non-bypass orchestrator can. What bast DID verify is
static (below the list). The build is static-clean; the semantic gate runs at first
orchestrator launch (auri/sulin's, the same gate as land sign-off).

Static build-verification bast ran (web-free, bypass-safe):
`bash -n` clean; `--profile orchestrator` assembles `claude … --permission-mode
default --allowedTools 'Read,Grep,Glob,WebSearch,WebFetch,Bash(bus *),Agent,Task,
Workflow'` with **no** `--dangerously-skip-permissions`; netns guard `-n "$netns"`
so the empty-netns orchestrator never mis-enters the `/run/netns/` cage path.

1. **Gated-write semantics (local).** Session with `--permission-mode default
   --allowedTools 'Read,Grep,Glob'`: confirm an `Edit` attempt **prompts** (not a
   silent apply, not a hard-deny) — i.e. unlisted ⇒ prompt. If `--allowedTools` is
   actually a hard allowlist, the orchestrator needs Edit/Write *in* it and we
   re-derive how to keep them gated. **Decides §4's flag shape.**
2. **Headless subagent write auto-denies (orchestrator-run).** Fan out a headless
   subagent that attempts an `Edit`/`Write` and confirm it **auto-denies and
   continues** (fail-closed) — never a silent apply, never a hang. This is the
   read-only-by-auto-deny guarantee (§5), the reason no custom type is needed.
3. **Web auto-approval propagates (orchestrator-run, not bypass).** With
   `WebSearch`/`WebFetch` pre-approved on the orchestrator session, confirm a
   background subagent's web call **auto-approves** (not auto-denied) so research
   actually works — and, separately, that a NON-pre-approved write attempt
   **auto-denies and continues** (no hang). Run by the auto-mode orchestrator once
   it exists; a bypass peer must not (web-under-bypass).
4. **`ultracode` took effect (orchestrator-run).** Confirm the launched
   orchestrator REPORTS `ultracode` / workflow-orchestration-ON and `xhigh` effort —
   i.e. `CLAUDE_CODE_EFFORT_LEVEL=ultracode` drove startup and was **not** overridden
   (this is the flag>env precedence check: clearing `--effort` let the env win). If
   it reports `high`, the `--effort` clear didn't take and the name-based flag is
   still leaking in. **Confirms §4's ultracode fold.**

## 8. v1 scope

**In:** the `orchestrator` profile case in `agent-launch` (§4 flags, **non-bypass —
forbidden-by-mechanism per §3.1**), which pre-approves `WebSearch`/`WebFetch` (+
`Bash(bus *)`) so subagent web research doesn't auto-deny; the §7 first-launch gate;
this doc. **No custom subagent type, no `.claude/agents` materialize** — read-only
is by auto-deny (§5), nothing to ship beyond the profile case.

**Explicitly OUT / deferred:**
- **Custom subagent type / `agents-shared` materialize NOT needed** — read-only is
  by auto-deny (§5); the Workflow script sets each subagent's model per `agent()`.
- **Worktree isolation NOT needed.** Only the single in-pane orchestrator writes,
  in its own jj workspace — subagents never land a write, so the parallel-write
  jj-workspace-vs-git-worktree unknown does not arise for v1.
- **SEC-1 nixos module NOT a dependency** — the auto-gate (non-bypass orchestrator)
  is the primary defense; the netns cage is optional defense-in-depth.
- **Nested subagent fan-out** — leaf-only v1 (§6); the orchestrator is sole fan-out.

## 9. Rollout

Config-via-relaunch, as always: the `agent-launch` `orchestrator` case is the only
artifact — land it to `main`, relaunch picks it up by construction. No subagent
definition to materialize. No live working-copy edits.
