# Design Philosophies for Agent Networks

Tools are downstream of philosophy. Picking tmux vs zellij, or Beads vs JSONL, is a small decision compared to deciding what an agent network *is*. The same toolkit builds wildly different systems depending on the philosophy underneath. This doc names the philosophies so we can pick deliberately.

## The axes that distinguish philosophies

Before naming philosophies, name the axes. Every design picks a point on each:

- **Authority** — where decisions come from. Central orchestrator, peer negotiation, human-in-the-loop, or emergent from rules.
- **Coupling** — how agents share information. Direct messages, shared workspace, through an orchestrator, or not at all.
- **Specialization** — are agents general or role-specific. Identical workers, fixed roles, or dynamic role assumption.
- **Synchrony** — when agents work. Sequential pipeline, parallel free-for-all, turn-taking, or event-driven.
- **Memory** — where state lives. Per-agent only, shared blackboard, external system, or the human's head.
- **Failure recovery** — what happens when an agent gets stuck or wrong. Retry, hand off, escalate to human, or vote.

Most "philosophies" are just consistent answers across these axes. Naming the axes lets you mix-and-match deliberately instead of inheriting a whole stack because you liked one feature.

## Philosophy 1 — The Conductor (hierarchical orchestration)

One agent owns the plan. Workers execute, report back, never talk to each other directly. The conductor decomposes work, assigns it, integrates results.

- **Axes:** central authority; coupling through orchestrator; workers identical or role-specific; memory lives with conductor.
- **Good when:** the work has a clear plan, the conductor's context can hold the whole picture, latency is fine.
- **Bad when:** the conductor's context becomes the bottleneck, the plan needs to adapt mid-flight based on what workers discovered, or you want workers to react to each other.
- **Examples:** Tmux-Orchestrator, most "manager + workers" demos, Claude Code's native agent teams.

## Philosophy 2 — The Swarm (peer queue)

Agents are interchangeable. Work sits in a queue; agents pull, claim, complete, repeat. No agent knows about another. Coordination is emergent from the queue's rules.

- **Axes:** no authority — the queue is the law; no direct coupling; identical workers; memory in the queue.
- **Good when:** work is genuinely parallel, tasks are independent or shallowly dependent, scaling means just spawning more agents.
- **Bad when:** tasks need rich handoffs ("here's the context, watch out for X"), or "next task" depends on synthesizing across completed ones.
- **Examples:** Claude Code Agent Farm; anyone who built `mv tasks/todo/X tasks/wip/agent-3/X`.

## Philosophy 3 — The Assembly Line (pipeline)

Fixed stages, work flows through. Stage 1 takes raw input, stage 2 transforms, stage 3 reviews, stage 4 ships. Each stage is an agent or pool; the order is structural.

- **Axes:** authority lives in the pipeline definition; stage-to-stage handoff only; high specialization; memory flows with the work item.
- **Good when:** the work has a real sequential shape (research → design → implement → review), and you want clean separation of concerns.
- **Bad when:** stages need to iterate (review fails, send back), or when the actual shape isn't linear.
- **Examples:** CI/CD-flavored agent systems; "research agent → coder agent → reviewer agent" patterns.

## Philosophy 4 — The Blackboard (shared workspace)

A central artifact — markdown file, database, document — every agent reads and writes opportunistically. No assigned roles, no fixed flow. Agents notice what needs doing and contribute.

- **Axes:** authority distributed by attention; coupling through the blackboard only; soft specialization; memory *is* the blackboard.
- **Good when:** the problem is open-ended (research, design), the right next step isn't predictable, you want "what should someone do next?" to be a queryable state.
- **Bad when:** you need predictable throughput, or when contention on the blackboard becomes the bottleneck (everyone reading the same 5000-line file).
- **Examples:** Beads (structured blackboard); classical AI blackboard systems; any `NOTES.md` two agents both edit.

## Philosophy 5 — The Specialist Team

Distinct, named roles (researcher, implementer, reviewer, librarian). Agents collaborate the way humans on a team do — meetings, handoffs, escalation paths. Closer to mirroring an org chart than to any computer-science model.

- **Axes:** authority by role and convention; rich peer coupling, often through messages; maximal specialization; per-role memory plus shared docs.
- **Good when:** the work benefits from specialization (different prompts, tool access, review standards) and the team is small enough that everyone knows what everyone else does.
- **Bad when:** roles become silos, or "who should do this?" overhead exceeds the work itself. Three-agent teams work; thirty-agent teams become bureaucracies.
- **Examples:** Gas Town (Mayor/Polecats/Witness/Crew); most multi-agent demos pitched as "AI software team."

## Philosophy 6 — The Cockpit (human-in-the-middle)

The human is the orchestrator. Agents are sophisticated tools. The system's job is to give the human maximum leverage — spawn agents, watch their work, interrupt, take over, hand back. No agent-to-agent communication; the human is the only integrator.

- **Axes:** human authority always; no inter-agent coupling — everything routes through the human; any specialization; memory in the human's head plus per-agent transcripts.
- **Good when:** the human is the expert and wants AI as force-multiplied focus, the work needs judgment the agents can't be trusted with, or you don't yet know the right delegation pattern.
- **Bad when:** the human becomes the bottleneck — five agents waiting for input means the human is doing five jobs.
- **Examples:** `claude-bus` as currently scoped; tmux-attach workflows generally; IDE-with-Copilot scaled to N agents.

## Philosophy 7 — The Actor Mesh (isolated, message-passing)

Agents are sealed processes that communicate only by messages. No shared state, no central authority. Coordination emerges from message patterns. Erlang/OTP intuitions applied to LLM agents.

- **Axes:** no authority; explicit messages only; any specialization; strictly per-agent memory.
- **Good when:** you need real isolation (one agent's failure shouldn't poison others), or you're scaling across machines.
- **Bad when:** the "what protocol do they speak?" overhead eats the project. Most LLM-agent work doesn't need actor-level isolation.
- **Examples:** A2A protocol; MCP-bus designs; anything pitched as "agents as microservices."

## How to read this catalog

You don't pick one. Real systems blend: a Cockpit at the top (human picks what to spawn) over a Swarm in the middle (workers pull from a queue) over an Assembly Line for one specific repeating workflow. The catalog is for naming what you're doing, not for choosing a single team jersey.

The diagnostic question when you're confused: **where does the next decision come from?**

- If it's the human → Cockpit.
- If it's a queue's rules → Swarm.
- If it's an agent looking at a shared doc → Blackboard.
- If it's a fixed plan → Conductor or Pipeline.
- If you can't answer, the system has no spine yet — design that first.

## Implications for `claude-bus`

The stated goal — observability across panes plus "user can jump into and help subagents" — anchors hardest in **Cockpit**. The interesting design question is what philosophy lives *underneath* the Cockpit for the moments when the human isn't actively steering. Three coherent answers:

- **Cockpit over nothing.** Agents are independent and idle when the human isn't engaged. Simplest. (No shared coordination.)
- **Cockpit over Swarm.** A queue exists; agents pull work when the human isn't directing them. Human can both attach to an active agent and edit the queue.
- **Cockpit over Blackboard.** A `NOTES.md` or similar grows over time; agents read it on each turn and contribute. Memory accumulates even when the human isn't watching.

Each is a real design. Choosing between them is choosing what `claude-bus` *is* — more than any tool choice will be.
