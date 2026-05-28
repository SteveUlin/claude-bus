# Git hooks

Repo-managed git hooks. Currently one:

- `prepare-commit-msg` — appends a `Co-Authored-By: <agent-id>` trailer
  when `$CLAUDE_BUS_AGENT_ID` is set, so per-agent authorship shows up
  in `jj log`, `git blame`, and the GitHub UI. No-op for regular git
  users (no env var, no trailer).

## Install — once per clone

```bash
git config --local core.hooksPath settings/git-hooks
```

That's it. `core.hooksPath` makes git look in this directory instead
of `.git/hooks/`. The hooks live in the repo so every agent picks up
new ones automatically.

To revert: `git config --local --unset core.hooksPath`.

## Why this directory and not `settings/hooks/`

`settings/hooks/` holds Claude Code hooks (event-driven scripts named
after `.claude/settings.json` events). Git hooks have a separate
contract — git invokes scripts by exact filename (`prepare-commit-msg`,
`pre-commit`, etc.) without extensions. Keeping them in their own
directory avoids name collisions and keeps the two harnesses
visually distinct.
