# Prior-art index

Research scaffolding for decisions that have since landed in code, the broker
spec, or `roles/`. The full surveys were removed in the Phase-4 doc-corpus
cleanup to keep the living-doc set lean; **their complete text remains in git
history** (`jj file show <rev> docs/<name>.md`). This index preserves the
pointer — what each survey covered and where its decision lives now.

| Survey (removed) | Covered | Outcome / where it landed |
|---|---|---|
| `human-agent-interaction.md` | Focus-aware mailbox for human↔agent direct conversation; focus-as-presence vs sentinel | **Superseded** — the implementation chose sentinel-only presence (`[bus-attach]`/`[bus-detach]`). |
| `mailbox-design-space.md` | Total design space for the mailbox/delivery layer | Folded into the broker: append-only topic logs + per-(topic,consumer) cursors + in-flight tracker. See `docs/broker-spec.md`. |
| `binary-log-formats.md` | Survey of binary append-only log formats from the ecosystem | Informed the `$STATE/topics/<name>.log` v4 wire format. |

Three further surveys (`modern-agent-techniques.md`, `observability-research.md`,
`delivery-alternatives.md`) are cited by-section across the frozen `deep/`
references and are retained with that corpus pending the post-Phase-2
regeneration.
