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
settings.json. Consequence that shapes the design: we **cannot** put `deny: Edit`
in shared `settings.json` to make subagents read-only, because that would also
block the *orchestrator's* gated writes (and break the rest of the fleet — one
ruleset per session). **The per-subagent `.claude/agents/*.md` allowlist is the
only mechanism, so materializing it is REQUIRED, not optional.**

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

## 5. Subagent envelope spec (`.claude/agents/*.md` + new materialize surface)

The orchestrator's Workflow `agent()` / Task calls resolve `agentType` to a
definition under `.claude/agents/`. Canonical source = a NEW tracked dir
`settings/agents-shared/*.md`, materialized per-launch to the workspace's
`.claude/agents/*.md` — **the same frozen-copy pattern as `settings/hooks/`**
(`rm -rf` then fresh-copy from landed `main`; see the #15 frozen-copy invariant).

A read-only worker definition, e.g. `settings/agents-shared/ro-worker.md`:

```yaml
---
name: ro-worker
description: Headless read-only research/coordination worker. No file writes.
tools: Read, Grep, Glob, Bash, WebSearch, WebFetch
---
You read, search, fetch, and report. You cannot edit or write files —
surface findings to the orchestrator, which holds the (gated) write.
```

- `Edit`/`Write`/`NotebookEdit` omitted ⇒ unavailable ⇒ no write, no prompt, no hang.
- `Bash` is allowlisted but the **session** `permissions.allow` still scopes
  *which* Bash (e.g. `Bash(bus *)`) auto-approves vs prompts — and a headless
  subagent prompt hangs, so keep subagent Bash to the pre-approved safe set only.
- **Leaf-only for v1:** the definition deliberately OMITS `Agent`/`Workflow` (see
  §6) — subagents do not fan out further; the orchestrator is the sole fan-out point.

## 6. Failure-mode backstop, and the nesting gap

**Backstop (defense in depth):** even if a subagent definition is misconfigured
and `Edit` slips into its `tools:`, a headless write attempt under a gated mode
**HANGS waiting for TTY input** (confirmed: claude-code GH #56540, #28482 — no
timeout, no auto-deny, no silent write). So a misconfig fails *loud and stuck*,
never *silent and written*. Read-only is enforced **by config AND backstopped by
failure** — the safe direction.

**Nesting gap (SURFACED — undocumented):** whether a subagent spawned *by a
subagent* (nested `Agent`/`Workflow`) inherits the parent **subagent's**
restrictions or resets to the **main session's** write-capable mode is **not
documented**. If it resets, a 2nd-level headless subagent could attempt a write
and hang. **v1 mitigation:** leaf-only subagents (§5) — no `Agent`/`Workflow` in
the subagent allowlist — sidesteps the gap entirely. Revisit only if nested
fan-out is needed, and then only after an empirical check.

(This narrows auri's spec, which listed `Agent`/`Workflow` in the subagent set.
Recommend dropping them for v1; the cost is "subagents can't fan out," which the
orchestrator-as-sole-fan-out-point already assumes.)

## 7. Pre-land empirical GATE (SEC-1-style: verify, don't trust the docs)

Three load-bearing claims get a tiny local check before land — the docs are
authoritative but flagged uncertainties, and the SEC-1 GATE precedent stands:

1. **Gated-write semantics.** Launch a throwaway session with
   `--permission-mode default --allowedTools 'Read,Grep,Glob'` and confirm an
   `Edit` attempt **prompts** (does not silently apply, does not hard-deny) —
   i.e. unlisted ⇒ prompt. If `--allowedTools` is actually a hard allowlist
   (only-these), the orchestrator needs Edit/Write *in* `--allowedTools` and we
   re-derive how to keep them gated. **Decides §4's flag shape.**
2. **Subagent allowlist enforcement.** A `.claude/agents/ro-worker.md` with no
   `Edit` in `tools:`, invoked via Workflow `agentType: 'ro-worker'`, genuinely
   cannot edit — the tool is absent, not prompted. **Confirms §5.**
3. **Headless write hangs (backstop real).** A deliberately misconfigured
   subagent (Edit in `tools:`) attempting a write under the gate **hangs/errors**
   rather than silently writing. **Confirms §6's backstop.** (Bounded with a
   timeout so the gate test itself can't wedge the fleet.)

## 8. v1 scope

**In:** the `orchestrator` profile case in `agent-launch`; `settings/agents-shared/`
canonical dir + its materialize to `.claude/agents/` (frozen-copy, like hooks);
one `ro-worker` subagent definition; the §7 gate; doc.

**Explicitly OUT / deferred:**
- **Worktree isolation NOT needed.** Only the single in-pane orchestrator writes,
  in its own jj workspace — read-only subagents never write, so the parallel-write
  jj-workspace-vs-git-worktree unknown does not arise for v1.
- **SEC-1 nixos module NOT a dependency** — auto-gate is primary; the netns cage is
  optional defense-in-depth.
- **Nested subagent fan-out** — gated on the §6 nesting question; leaf-only for now.

## 9. Rollout

Config-via-relaunch, as always: land `settings/agents-shared/*` +
`settings/claude-settings.json` (if the safe-allow set moves there) +
`agent-launch` to `main`; relaunch picks it up by construction (materialize from
landed `main`). No live working-copy edits.
