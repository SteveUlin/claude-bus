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
mechanism for the dangerous post-web step (a write).

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

**Answer: YES, separable — it is NOT session-wide-only.** (Authoritative, via
claude-code-guide; load-bearing claims gated empirically in §7.)

- A subagent definition's `tools:` frontmatter is an **independent allowlist**,
  *not* inherited from the parent session's `--permission-mode`. Official docs:
  each subagent "runs in its own context window with … specific tool access, and
  **independent permissions**."
- Enforcement is **by unavailability**: if `Edit`/`Write` are omitted from a
  subagent's `tools:`, the tool is not in its context at all — it can never even
  *attempt* a write, so it never reaches a permission prompt. No hang, no write.
- Therefore writes stay at the **answerable in-pane orchestrator**; headless
  subagents are read-only **by config**.

**The one thing that is session-wide:** `settings.json` `permissions.allow`/`deny`
apply to the whole session uniformly — there is **no per-agent block** in
settings.json. Consequence: we **cannot** put `deny: Edit` in shared
`settings.json` to make subagents read-only, because that would also block the
*orchestrator's* gated writes (and break the rest of the fleet — one ruleset per
session). Per-subagent restriction must come from the **subagent layer**, not the
session layer — see §5, where a built-in agentType supplies it with no config at all.

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

## 5. Subagent envelope — built-in `Explore` agentType (primary)

**v1 mechanism: Workflow `agent(..., {agentType: 'Explore'})`.** The built-in
`Explore` type is bounded to a read-only + web tool set **BY TYPE** — no
allowlist, no custom definition, no materialize. Confirmed from the local
Agent/Workflow tool spec (no web lookup): `Explore` = "All tools except `Agent`,
`ExitPlanMode`, `Edit`, `Write`, `NotebookEdit`." So it HAS `Read`/`Grep`/`Glob`/
`Bash`/`WebSearch`/`WebFetch` and CANNOT `Edit`/`Write` — exactly the subagent
envelope (read-only + web), and a headless write is structurally impossible (the
tool is absent by type). It also excludes `Agent`, closing one nesting vector
(§6).

**Why this beats a custom `.claude/agents/*.md` definition for v1:**
- **No new structural surface.** `Explore` ships with the harness — the orchestrator
  just passes `agentType: 'Explore'` in its `agent()` calls. **The
  `settings/agents-shared/` → `.claude/agents/` materialize is NOT needed for v1.**
  (That was my earlier "REQUIRED" constraint; `Explore` dissolves it.)
- **Enforced by type, not by a frontmatter list** that could be misedited.

**OPEN NUANCE — parked on the sanctioned claude-code-guide answer (auri owns; do
NOT web-search it):** in an **auto/`--permission-mode default`** session, does a
HEADLESS `Explore` subagent's `WebSearch`/`WebFetch` call get auto-approved, or
does it PROMPT — and therefore hang (headless can't answer)? `Edit`/`Write` hang
is already moot (absent by type), but if web tools PROMPT under default mode, a
headless `Explore` wedges on its first search. The design branches on this:
- **If web auto-approves under default mode** → `Explore` works as-is, zero config.
- **If web prompts** → either pre-approve `WebSearch`/`WebFetch` for the
  orchestrator's subagent scope (if that's expressible) or accept that web-reading
  subagents need a non-prompting path. Decide once guide answers.

**Fallback (only if `Explore` is unusable):** a custom `settings/agents-shared/
ro-worker.md` (`tools: Read, Grep, Glob, Bash, WebSearch, WebFetch`, no Edit/Write)
materialized to `.claude/agents/` via the frozen-copy pattern (`rm -rf` + fresh
copy from landed `main`, like `settings/hooks/`). Same read-only-by-unavailability
property, but reintroduces the materialize surface — so it's plan B.

**Bash note (both paths):** `Explore` has `Bash`, but the **session**
`permissions.allow` still scopes *which* Bash auto-approves (e.g. `Bash(bus *)`)
vs prompts — and a headless subagent that prompts hangs. So the orchestrator's
session must pre-approve any Bash the subagents need (the safe `Bash(bus *)` set);
anything outside it is the same parked web-prompt question.

## 6. Failure-mode backstop, and the nesting vector

**Backstop (defense in depth):** with `Explore`, `Edit`/`Write` are absent **by
type**, so a headless write is structurally impossible — no prompt, no hang, no
write. Even in the plan-B custom path, if `Edit` were misconfigured into a
subagent's `tools:`, a headless write attempt under a gated mode **HANGS waiting
for TTY input** rather than writing silently (a known headless behavior). So the
failure direction is always *loud-and-stuck*, never *silent-and-written* — the
safe one.

**Nesting vector (narrowed, not gone):** `Explore` **excludes `Agent`** by type,
so it cannot spawn a Task subagent — that nesting vector is closed structurally.
It does **not** exclude `Workflow`, so whether an `Explore` subagent could call
`Workflow` (and what permission set THAT child gets — parent-Explore's bounds, or
a reset to the orchestrator's write-capable mode) is the **one remaining nesting
question**. Whether nested `Workflow` even works from inside a subagent is itself
unconfirmed. **v1 mitigation:** treat `Explore` subagents as LEAVES — the
orchestrator is the sole fan-out point; don't rely on subagent-initiated
`Workflow`. Revisit only if nested fan-out is needed, after the guide answer.

## 7. Pre-land empirical GATE (SEC-1-style: verify, don't trust the docs)

Load-bearing claims get a tiny local check before land (SEC-1 GATE precedent).
**None of these require web access** — they're local session/subagent behavior.
The Explore-in-auto-mode web-prompt question is NOT in this gate: it's parked on
auri's sanctioned claude-code-guide answer (bypass peers must not web-probe it).

1. **Gated-write semantics.** Launch a throwaway session with
   `--permission-mode default --allowedTools 'Read,Grep,Glob'` and confirm an
   `Edit` attempt **prompts** (does not silently apply, does not hard-deny) —
   i.e. unlisted ⇒ prompt. If `--allowedTools` is actually a hard allowlist
   (only-these), the orchestrator needs Edit/Write *in* `--allowedTools` and we
   re-derive how to keep them gated. **Decides §4's flag shape.**
2. **`Explore` is read-only by type.** Invoke Workflow `agent(..., {agentType:
   'Explore'})` and confirm it has no `Edit`/`Write` available (a write attempt
   reports the tool absent, never a silent apply). **Confirms §5's primary path.**
3. **Web behavior under auto mode (PARKED — guide, not gate):** does a headless
   `Explore` `WebSearch`/`WebFetch` auto-approve or prompt-and-hang under
   `--permission-mode default`? Answered by auri's guide channel, then folded into
   §5. A bypass peer must NOT run this empirically (it's the web-under-bypass hole).

## 8. v1 scope

**In:** the `orchestrator` profile case in `agent-launch` (§4 flags); subagents via
built-in `agentType: 'Explore'` (§5 — NO new dir, NO materialize); the §7 local
gate; doc. Build starts after auri's spec review + the parked guide answer (§5).

**Explicitly OUT / deferred:**
- **`settings/agents-shared/` materialize** — NOT needed for v1; `Explore` supplies
  the read-only+web envelope by type. Only build it if the guide answer rules
  `Explore` unusable (the §5 plan-B fallback).
- **Worktree isolation NOT needed.** Only the single in-pane orchestrator writes,
  in its own jj workspace — read-only subagents never write, so the parallel-write
  jj-workspace-vs-git-worktree unknown does not arise for v1.
- **SEC-1 nixos module NOT a dependency** — auto-gate is primary; the netns cage is
  optional defense-in-depth.
- **Nested subagent fan-out** — `Explore` excludes `Agent`; the `Workflow`-nesting
  question (§6) stays deferred; leaf-only for now.

## 9. Rollout

Config-via-relaunch, as always: land the `agent-launch` `orchestrator` case (+
`settings/claude-settings.json` only if a safe-allow set moves there) to `main`;
relaunch picks it up by construction. The v1 subagent path (`Explore`) ships with
the harness, so there's nothing to materialize. No live working-copy edits.
