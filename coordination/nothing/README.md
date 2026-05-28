# nothing — the Cockpit baseline

No coordination beyond the bus itself. Agents have no shared state, no idle behavior, no protocol for talking to each other except by direct `bin/bus msg send` calls. They sit at their prompts until a message arrives (from sulin or another agent); they respond; they wait again.

This is the implicit pattern of the bus as built in steps 1-8. It exists as a named pattern so other patterns have a baseline to be compared with and so the framework documents the "zero" case explicitly.

## When this fits

- One agent at a time is doing real work; the others are tools waiting to be asked.
- The human is the only integrator. They route tasks, they collect results, they decide what's next.
- The work doesn't have a queue, a backlog, a shared document, or any cross-task context worth persisting.

## What it costs

- The human is the bottleneck. Five idle agents and one busy human means four agents are doing nothing useful.
- No memory between sessions or between agents. Anything an agent learned dies with its pane.
- Coordination beyond two agents gets cognitively expensive for the human.

## What's already built for this

Everything in steps 1-8. There is nothing to load, nothing to scaffold, nothing to teach agents beyond what `CLAUDE.md` already says about `bin/bus`.

## What's missing

- A way to start a "nothing-session" with N agents from a parameter, instead of hardcoding alice/bob in a layout. (Comes when we generalize layouts.)
- A standard demo scenario so any future pattern can be compared against this baseline. (Comes when scenarios are formalized.)
