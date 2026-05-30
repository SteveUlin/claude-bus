# Log retention & rotation (D1 + D2)

`$STATE` is now durable (XDG root, survives reboot). Two classes of log
that used to vanish on a `/tmp` wipe now grow unbounded:

1. **`events.jsonl`** — the advisory JSONL event stream every hook appends
   to. ~25 MB live today. It drives all agent-state derivation
   (`readAgents`), and the broker tails it for ACKs (`scanEvents`). It is
   **advisory**: the binary topic logs are the canonical system-of-record,
   so dropping old event lines is safe.
2. **Topic logs** (`$STATE/topics/<name>.log`) — the canonical binary
   append logs. `audit.log` (broker writes every escalation / doorbell /
   auto-clear here) and `work-queue` topics grow without bound. The
   registry already *declares* `retention_ms` + `max_record_bytes`
   (`topic_registry.h`) but **nothing enforced them** — dead config.

Both are trimmed in-tick by the broker's delivery loop. The broker is the
**only** writer to topic logs and runs RPC + tick on a single thread
(`rpc::Server::run`'s pselect loop), so an in-tick rewrite never races a
concurrent topic-log append. `events.jsonl` is the one exception — hooks
append to it concurrently — handled below.

## events.jsonl — tail-preserving rewrite (D1)

When `events.jsonl` exceeds `CLAUDE_BUS_EVENTS_MAX_BYTES` (default 16 MiB),
the broker rewrites it to keep only the most recent ~half (aligned to a
line boundary) via tmp-file + atomic `rename`. Why keep a tail rather than
truncate to empty:

- `readAgents` reads the **whole** file each call to derive per-agent
  state. A roll-to-empty would blank every agent's last-known state until
  each emitted a fresh event — visible amnesia in the monitor/deck. Keeping
  the recent tail preserves each agent's latest event.
- After the rewrite the broker sets `events_offset_` to the new EOF, so
  `scanEvents` resumes at the end and does **not** reprocess the retained
  tail. This matters: reprocessing an old `UserPromptSubmit` would
  positionally ACK a *newer* in-flight record (premature ack). The retained
  tail exists only for `readAgents`' full-file state read, never for the
  incremental ACK scan.

**Concurrency caveat (documented, accepted):** a hook append racing the
rename writes to the old (unlinked) inode and is lost. The window is a
single rename; the data is advisory; per-agent state self-heals on the next
event. The canonical topic logs are untouched.

## Topic logs — head trim with cursor rebase (D2)

Trimming a topic log drops the oldest records by **byte-slicing** the file:
copy `header (64 B) + bytes[cut_offset..EOF]` to a tmp file, `rename` over
the original. Record bytes are preserved verbatim (timestamps intact) — we
never re-`append`, which would restamp `sent_ms`.

Dropping `D = cut_offset - header` bytes from the head shifts every
surviving record's absolute byte offset down by `D`. Cursors and in-flight
trackers store **absolute** offsets, so each must rebase:

    new = max(header, old - D)

The broker rebases, per trimmed topic:
- every `$STATE/cursors/<topic>/*.cursor`
- every in-flight tracker's `cursor_after` whose `.topic` matches

### What gets cut — `planTrim` (pure, unit-tested)

`bus::retention::planTrim` decides `cut_offset` from record metadata
(`offset`, `next_offset`, `sent_ms`), the clock, and two limits:

- **Age** — `retention_ms` (per-topic, from the registry; `0` = no age
  limit). Drop leading records with `sent_ms + retention_ms < now`.
- **Size** — `CLAUDE_BUS_TOPIC_MAX_BYTES` (default 8 MiB; `0` = no cap).
  Drop oldest records until the kept span is ≤ cap.

Take whichever cuts more. Then the **delivery-guarantee clamp**:

- **agent-inbox / tui-commands** (guaranteed delivery): clamp
  `cut_offset ≤ min_cursor` — only records every consumer has already
  passed are eligible. **Undelivered mail is never dropped**, even if
  "expired." (An inbox with unread mail and no cursor has `min_cursor = 0`
  → clamps to header → no trim.) In-flight records sit at/after the cursor,
  so the clamp protects them too.
- **audit / pubsub / work-queue / blackboard / append-log** (fire-and-
  forget / advisory): no clamp — age/size expiry drops records regardless
  of consumption. This is the intended retention semantics and is what lets
  `audit.log` (no persistent consumer cursor) actually shrink.

`retention_ms` stays `0` (unbounded) for every auto-created topic today, so
D2 is **dormant by default** except the absolute `CLAUDE_BUS_TOPIC_MAX_BYTES`
safety cap. Set `retention_ms` on a topic to opt a stream into age-based
expiry.

## Why `parseFrom` is NOT migrated onto `TailReader` (D6 scope note)

D6 wired `scanEvents` + `maybeScanTokens` (newline-framed text tails) onto
the shared `TailReader`. `topic_log.cpp::parseFrom` shares the *invariant*
(resume-from-offset, refuse a torn tail) but not the *framing*: topic-log
records are length-prefixed binary, not newline-delimited, so
`TailReader::splitCompleteLines` (a `\n` splitter) cannot parse them.
`parseFrom` already refuses torn tails correctly (it stops when
`pos + rec_len > buf.size()`) and is unit-tested. Forcing it onto a
newline reader would add risk for zero correctness gain, so it stays.
