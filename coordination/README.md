# Coordination patterns

`coordination/` is a library of patterns that can be **layered on top of the bus** to give agents structured ways to interact beyond direct human routing. The bus mechanics in `bin/` are the substrate; each pattern here adds shared state, idle behaviors, conventions, or hook plumbing that lets agents coordinate in a particular shape.

This isn't a competition — patterns coexist. Pick the one (or combination) that fits your use case. Add new ones as new use cases come up.

## What a pattern looks like

A pattern lives in its own subdirectory. It supplies as much (or as little) as it needs:

- **`README.md`** — what this pattern is, what use cases it fits, what it costs.
- **`agent-prompt.md`** — markdown that gets concatenated into agents' CLAUDE.md when this pattern is in play. Teaches agents what state exists, how to read it, how to write to it, what to do on idle.
- **`hooks/`** — pattern-specific hook scripts that augment the shared hooks in `settings/hooks/` (e.g., a Stop hook that pulls the next queue item; a UserPromptSubmit hook that records to a shared notes file).
- **`scripts/`** — CLI helpers agents invoke. Mirror the bus tools in `bin/` (e.g., `claim`, `complete`, `note`).
- **`state/`** — initial files staged to a runtime location when the pattern is loaded.

A pattern can leave any of these out. The minimum is a README; everything else is opt-in based on what the pattern actually needs.

## Composing with the bus

The bus is always there. Patterns add on:

- **Cockpit baseline** (no pattern at all): agents act only on direct messages routed through the bus by sulin. This is `coordination/nothing/`.
- **+ shared queue**: agents pull tasks when idle.
- **+ shared notes**: agents read a common doc on each turn and contribute.
- **+ named mailboxes**: each agent has a maildir for asynchronous direct messages.
- **Combinations**: queue for tasks + notes for context + bus for direct steering.

The bus pattern (Cockpit, human-in-the-middle) doesn't conflict with any of these — sulin can always interject.

## Loading a pattern (TBD)

How a pattern actually gets activated for a session — concatenating its `agent-prompt.md` into CLAUDE.md, wiring its hooks into `.claude/settings.json`, initializing its state — is unresolved. The shape will emerge when the second pattern lands; for now the only pattern is `nothing`, which requires no loader.
