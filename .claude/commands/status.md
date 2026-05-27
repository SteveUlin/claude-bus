---
description: One-screen summary of fleet state across all agents
---

Compose a fleet-state snapshot. Run these in parallel:

- `bus agents --json` (registered agents)
- `bus state` (broker's lifecycle view)
- `bus events --since 10m` (recent activity)

Then produce a summary for sulin covering:

- Per-agent state (IDLE / WORKING / STUCK / HAS_MAIL), grouped by session.
- Anyone who looks stuck (no event in >2 min while WORKING).
- Anyone with pending mail (mail count > 0).
- Anything noteworthy on the `inbox-ops` tail.

Keep it under 15 lines. One short line per agent. No raw JSON. Skip
agents that are uninteresting (idle, nothing pending).
