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
| **Subagent** | a headless in-process worker (Workflow `agent()` / Task) | the orchestrator, at runtime | its `.claude/agents/<name>.md` `tools:` frontmatter + `agentType` |

auri's spec maps cleanly: "orchestrator = auto + write-gated" is the **session**
layer; "subagents = read-only allowlist, no prompt" is the **subagent-definition**
layer. They are configured in different places and — critically — can differ.

## 3. THE KEY QUESTION, answered: separation IS possible

> Can claude-code separate the orchestrator's permission set (auto + write-gated)
> from the subagents' set (read-only allowlist, no prompt)? Or is it only one
> session-wide `--allowedTools`/`--permission-mode`?

**Answer: YES, separable — BUT ONLY IF THE ORCHESTRATOR IS NON-BYPASS.**
(Authoritative via claude-code-guide, routed by auri — the sanctioned channel, no
web from this bypass peer; code.claude.com sub-agents + permission-modes.)

- Read-only subagents exist two ways: built-in `agentType: 'Explore'` (denies
  Edit/Write, allows Read/Grep/Glob/WebSearch/WebFetch), OR a **custom** type in
  `.claude/agents/*.md` with `tools:`/`disallowedTools:`. **Enforced by the
  TOOLSET** — the tool is simply absent, so a read-only subagent never even
  *attempts* a write, never prompts.
- **The headless-hang fear is UNFOUNDED (correcting my earlier draft):** background
  subagents **AUTO-DENY any permission prompt and CONTINUE** — fail-closed and
  graceful, they do **not** hang. (My prior "hangs loud-and-stuck" claim came from
  stale GH issues I should not have fetched; the authoritative answer supersedes it.)

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
    ;;
```

Network: the orchestrator needs web, so it is **not** caged to the
anthropic-only worker tier. Either no cage (auto-gate is the defense) or the
`research`/scholar squid allowlist tier — auri's call; default to **no netns
cage** for v1 since auto-gate is primary and SEC-1 is optional.

Semantics this relies on (gated in §7): under `--permission-mode default`,
`--allowedTools` *pre-approves* the listed tools while a tool in **neither**
`--allowedTools` nor `--disallowedTools` still **prompts**. (The `research`
profile uses both lists together, which already implies this three-way split:
allow = auto, disallow = hard-block, unlisted = prompt.)

## 5. Subagent envelope — a custom read-only type (auri's recommended profile)

Two valid mechanisms, both enforced by the TOOLSET (§3) and both holding **only
under a non-bypass orchestrator** (§3.1):

**(A) Custom read-only type — `settings/agents-shared/ro-worker.md` (RECOMMENDED).**
auri's specified profile. Explicit, stable toolset:

```yaml
---
name: ro-worker
description: Headless read-only research/coordination worker. No file writes.
tools: Read, Grep, Glob, WebSearch, WebFetch
disallowedTools: Edit, Write, NotebookEdit
---
You read, search, fetch, and report findings to the orchestrator,
which holds the (gated) write. You cannot edit or write files.
```

Materialized per-launch to the workspace's `.claude/agents/ro-worker.md` via the
**frozen-copy pattern** (`rm -rf` then fresh copy from landed `main`, exactly like
`settings/hooks/` — the #15 frozen-copy invariant). This is the new structural
surface in bast's lane. The orchestrator invokes it via Workflow `agent(...,
{agentType: 'ro-worker'})`. Chosen over Explore because the toolset is **explicit
and version-stable** (Explore's set is harness-defined and could drift), and it
omits `Bash` entirely — no Bash-scoping question at all.

**(B) Built-in `agentType: 'Explore'` — zero-config alternative.** Bounded by type
to "all tools except `Agent`, `ExitPlanMode`, `Edit`, `Write`, `NotebookEdit`"
(from the local Agent/Workflow spec) → read-only + web, no write, no `Agent`
nesting, **no materialize surface**. The lighter option if we'd rather not add the
`agents-shared` dir. Difference from (A): Explore also carries `Bash`/`Workflow`
(see the Bash + nesting notes in §6). **Decision (A vs B) is auri's at review.**

**Web tools must be PRE-APPROVED, or research silently fails (the auto-deny
consequence).** Since background subagents **auto-deny** any prompt (§3), if
`WebSearch`/`WebFetch` would *prompt* under the orchestrator's default mode, a
subagent's web call is silently denied — it won't hang, but it won't fetch either.
So the orchestrator session must **pre-approve** `WebSearch`/`WebFetch` (in its
`--allowedTools` / `permissions.allow`); subagents inherit that auto-approval and
can actually research. Same logic for any `Bash` a subagent needs (path B):
pre-approve the safe `Bash(bus *)` set or it auto-denies. Verifying the
auto-approve actually fires is a §7 check the (non-bypass) orchestrator runs — not
this bypass peer.

## 6. Failure mode, Bash, and the nesting vector

**Failure mode (corrected — fail-closed, graceful):** with a read-only subagent,
`Edit`/`Write` are absent, so a write is never attempted. And if anything *would*
prompt (a write somehow reachable, or an un-pre-approved web/Bash call), the
background subagent **AUTO-DENIES and CONTINUES** — it does **not** hang. So the
failure direction is *silently-denied*, never *silent-write* and never *wedged*.
The earlier "hangs loud-and-stuck" framing is retracted (§3). The flip side is the
§5 auto-deny consequence: a needed tool that isn't pre-approved gets silently
skipped — so pre-approve the subagents' real needs (web, `Bash(bus *)`).

**Bash (steer subagents to native read tools):** path A (`ro-worker`) omits `Bash`
entirely — no scoping question. Path B (`Explore`) carries `Bash`; the session
`permissions.allow` scopes which Bash auto-approves (`Bash(bus *)`), and anything
else auto-denies (harmless). Under §4 only `Bash(bus *)` is granted, so a subagent's
arbitrary `Bash` (e.g. `rg`/`find`) auto-denies and the read silently fails.
**Mitigation: the orchestrator's prompt must steer subagents to the granted native
tools — `Read`/`Grep`/`Glob` (all auto-approved, both paths) — over `Bash` for
read-only work.** Path A enforces this structurally (no Bash); path B relies on the
prompt. A line in the orchestrator role/prompt covers it.

**Nesting vector:** path A omits `Agent`/`Workflow` → no nesting at all. Path B
(`Explore`) excludes `Agent` but carries `Workflow`, so "could an Explore subagent
call `Workflow`, and what perms would that child get" is an open question — but note
§3.1: a non-bypass orchestrator means even an inherited mode is the *default* (gated)
one, not bypass, so a nested child still couldn't silently write. **v1 mitigation
(both paths):** treat subagents as LEAVES; the orchestrator is the sole fan-out
point. Path A enforces this structurally.

## 7. Pre-land empirical GATE (SEC-1-style: verify, don't trust the docs)

The mechanism is now authoritative (guide), so the gate just confirms it fires as
documented before land (SEC-1 precedent). **HONEST FINDING (bast, build time): a
bypass peer cannot actually run any of these** — check 1 needs *interactive*
prompt observation (headless auto-denies, so the prompt can't be seen), and check 2
would require spawning a web-capable `Explore` subagent = web-under-bypass (the very
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
2. **Read-only subagent is write-incapable (local).** Invoke the chosen subagent
   type (`ro-worker` or `Explore`) and confirm `Edit`/`Write` are absent — a write
   reports the tool unavailable, never a silent apply. **Confirms §5.**
3. **Web auto-approval propagates (orchestrator-run, not bypass).** With
   `WebSearch`/`WebFetch` pre-approved on the orchestrator session, confirm a
   background subagent's web call **auto-approves** (not auto-denied) so research
   actually works — and, separately, that a NON-pre-approved write attempt
   **auto-denies and continues** (no hang). Run by the auto-mode orchestrator once
   it exists; a bypass peer must not (web-under-bypass).

## 8. v1 scope

**In:** the `orchestrator` profile case in `agent-launch` (§4 flags, **non-bypass —
forbidden-by-mechanism per §3.1**); the read-only subagent type (§5 — path A
`ro-worker` + its `settings/agents-shared/` → `.claude/agents/` frozen-copy
materialize is the recommended build; path B `Explore` if auri prefers zero
materialize); pre-approve `WebSearch`/`WebFetch` (+ `Bash(bus *)` for path B) on
the orchestrator session so subagent web research doesn't auto-deny; the §7 gate;
doc. Build starts after auri's spec review.

**Explicitly OUT / deferred:**
- **Worktree isolation NOT needed.** Only the single in-pane orchestrator writes,
  in its own jj workspace — read-only subagents never write, so the parallel-write
  jj-workspace-vs-git-worktree unknown does not arise for v1.
- **SEC-1 nixos module NOT a dependency** — the auto-gate (non-bypass orchestrator)
  is the primary defense; the netns cage is optional defense-in-depth.
- **Nested subagent fan-out** — leaf-only v1 (§6); path A omits Agent/Workflow
  entirely, path B's `Workflow`-nesting stays deferred.

## 9. Rollout

Config-via-relaunch, as always: land the `agent-launch` `orchestrator` case +
(path A) `settings/agents-shared/ro-worker.md` + its materialize step to `main`;
relaunch picks it up by construction (the `.claude/agents/` copy frozen at launch,
like `settings/hooks/`). Path B (`Explore`) ships with the harness — nothing to
materialize. Either way, no live working-copy edits.
