# Journal — follow-ups punch-list

Durable record of changes identified while reviewing the `bus::Journal` kernel
(the `topic_log`→`journal` refactor). Survives context compaction. Delete items
as they land; delete the file once it's empty.

## Status of the work itself

The journal refactor is **GREEN but UNLANDED**. It lives in jj changes on top
of `main` (`3f174ff8`):

- `vputoqun` — wire v5 (CRC32C, torn-write rollback, durability tier, seq ids)
- `ynktyxrq` — this refactor (rename → `bus::Journal`, opaque envelope, wire v6
  both-ends framing, reverse `tail()`, opaque `Cursor`)

Both are **unpushed**. `main` still has `topic_log` v4, so the running fleet
sees the old kernel until this lands. Built clean, 172 unit tests pass.

**Decision pending:** land it (fleet relaunches onto `Journal`), or keep it as a
review/learning branch.

---

## P1 — the cursor migration is only half done (correctness-relevant)

The C2 cut introduced opaque `bus::Cursor`, the `Journal` cursor-store API
(`consumerCursor` / `ack` / `lastAckedId`), token persistence, and
`rebaseCursor` — and migrated **some** call paths:

- ✅ `broker.cpp` drain / fetch / drop (`~804`–`1045`) — uses the new API.
- ✅ `delivery.cpp` in-flight tracker + agent-inbox ack (`239`, `262`, `309`,
  `1757`) — uses `cursorToToken`/`cursorFromToken`/`ack`/`rebaseCursor`.

But the goal — *no byte offset leaves the kernel* — is **not met**:

- [ ] `delivery.cpp`'s per-topic dispatch / cursor-advance loops (~25 sites:
  `537`, `565`, `595`, `617`, `692`–`728`, `840`–`860`, `1093`, `1165`, `1256`–
  `1289`, `1342`–`1350`, `1414`, `1647`, …) still call `bus::cursorPath` /
  `readCursor` / `advanceCursorMonotonic` / `writeCursor` and pass
  `rec.next_offset` — a **raw int64** — directly.
- [ ] `Record` still exposes public `offset` / `next_offset` int64 fields
  (`journal.h:96`). The "internal use only — use the Cursor API" comment is
  aspirational; the offsets are in active external use.
- [ ] The cursor free functions (`readCursor`/`writeCursor`/`cursorPath`/
  `advanceCursorMonotonic`/`lastIdPath`/`readLastId`/`writeLastId`) are still
  **public** in `journal.h:211`–`232`.

**Closure:** migrate the remaining `delivery.cpp` sites to
`journal.consumerCursor(consumer)` / `journal.ack(consumer, rec.cursor_after)`;
remove `Record::offset`/`next_offset` from the public struct (or make them
private to the wire parser); make the cursor free functions internal to
`journal.cpp`. Then the opaque-cursor invariant actually holds and the compiler
enforces it. This is the live mail path — do it as its own verified pass.

  **Target `Record` shape** (collapses the redundant position): a Record stores
  one position, not three. Today it carries `offset` + `next_offset` (int64) +
  `cursor_after` (= `Cursor{next_offset}`) — one fact, three encodings. Since
  `payload` is a field and the overhead is a fixed constant, the boundaries are
  inter-derivable (`after = start + payload.size() + overhead`). End state:
  `Record { int64 append_ms; string payload; Cursor position; }` with a derived
  `cursorAfter()` (kernel-side, since Cursor arithmetic is kernel-private).
  `retention` derives its boundaries from the cursor + payload size. `id` stays
  cached (recomputing `format(append_ms,seq)` per read is the rule-2 exception).

  **Target cursor API** (retires the free-function pile). The public free
  functions (`readCursor`/`writeCursor`/`advanceCursorMonotonic`/`cursorPath`/
  `lastIdPath`/`readLastId`/`writeLastId`) are pre-encapsulation leftovers that
  duplicate `Journal`'s private helpers; they survive only because delivery's
  ~25 sites still call them. End state: the public surface is the `Journal`
  member API + the `Cursor` value type, no path-taking frees.
    - Store ops (read/advance a consumer's persisted cursor) live on `Journal`
      (`consumerCursor(consumer) -> Cursor`, `ack(consumer, Cursor)`,
      `lastAckedId`) — they need (state_root, name, consumer) context and own the
      advance-only invariant; they do NOT belong on the `Cursor` value.
    - Token (de)serialize (value-intrinsic, no store) may move onto `Cursor`
      (`cursor.toToken()` / `Cursor::fromToken`) from the current `Journal`
      statics — optional cleanup.
    - `isStaleVersion`/`journalPath` → internal helpers or `Journal` statics;
      callers using `Journal{state_root, name}` never need `journalPath`.

- [ ] **Add a cursor-driven reader/range** (the ergonomic vehicle for the above).
  Today consumers read via `peek(start_offset)` — a raw int64 — which is *why*
  they still touch offsets. Add an ephemeral forward walk built over the durable
  Cursor, e.g. `read(Cursor, limit)` / `for (rec : journal.from(cursor))`. Two
  layers: `Cursor` = the saved position (durable noun); the range = the live walk
  (ephemeral verb). This is both the nicer reader API and what lets `delivery`
  stop handling raw offsets.

- [ ] Reconcile `retention.h::rebaseCursor` (a free `int64` function, `:94`)
  with `Journal::rebaseCursor` — one of them is now redundant.

---

## P2 — decided decoupling (same "project leaks into the kernel" class as the rename)

- [ ] **Magic `BUS\0` → a format signature.** It's the file's magic number
  (offset 0, identifies the format like PNG/ELF/SQLite), but it names the
  *project*, not the *format* — the same leak `TopicLog` was. A domain-agnostic
  journal should carry a journal-format signature. ~4 bytes; `makeFileHeader`
  in `journal.cpp` + the wire comment in `journal.h`.
- [ ] **Extension `.log` → an owned one.** `.log` is generic (Kafka segments and
  RocksDB WALs both use it); `.jnl` is taken (Ingres + ~21 formats). Coin a
  signature + extension together so they identify one format. `.jrnl` had no
  established owner; or invent a 4-letter coinage. Only `journalPath` builds it.

> Note: pick the magic + extension together as one "format identity" decision.

---

## P3 — leanness / taste (no behavior change)

- [x] **Drop `payload_len`** — DONE (build + 172 tests green; overhead = 30, a
  named `kRecordOverhead` constant). Research (LevelDB,
  RocksDB, protobuf, MessagePack, systemd journal, BSON, TLV): no format with a
  fixed-size header stores both a total and a payload length for the same span;
  the legit keep-both reasons (variable header, validation cross-check, padding)
  don't apply here. Payload is derived as `record_len − overhead` (overhead =
  30, a named constant). −4 bytes/record. Version stays 6 (unlanded; no v6 files
  exist in production).
  (Contrast: the front/trailer `record_len` pair *looks* redundant but isn't —
  two access directions + the equality is the torn-frame check. Keep that.)
- [ ] **Right-size lengths to `u32`** — `record_len` (front + trailer) is `u64`
  but the record cap is 1 MiB, which fits `u32`. Saves 8 bytes/record. Minor.

---

## P4 — deferred mechanical cleanups (low risk)

- [ ] **Wire `journalPath()` into the 13 remaining hand-built
  `state_dir + "/topics/" + name + ".log"` strings** in `delivery.cpp` /
  `broker.cpp`. The helper exists; the call sites weren't swapped.
- [ ] **Two stale `topic_log` comments** — `sub_inbox.cpp`, `tail_reader.h`
  still mention the old name in prose.
- [ ] **Torn-write rollback has no test.** The `ftruncate`-on-short-write path
  in `append()` is untested; it needs a write-hook seam
  (`setWriteHookForTesting`) to fault-inject a short write.

---

## P5 — open questions (taste calls surfaced in review)

- **Epoch placement** — currently a field of `bus::msg::Envelope` (the kernel
  has no epoch concept). Reasoning: "is a pre-reboot record stale?" is the
  reader's policy, not the log's. Confirm, or move it back to a kernel field.
- **Reverse-scan / random access** — `tail(N)` walks up from EOF (forward-
  fallback on a torn tail). No sparse index for "jump to time T" — defer until
  logs are large enough that the forward scan hurts; it's a format-independent
  sidecar when needed, so no need to decide now.
- **Reads slurp the whole file; fd is per-operation** — `peek`/`dump`/`tail`
  all call `readAll`, so even `tail(N)` is O(filesize) on I/O despite its O(N)
  walk. Every method `::open`s fresh (`Journal` is a stateless per-call value
  type). Per-op open is *correct* as-is: `trimHead` renames the file (atomic
  tmp+rename), so a cached fd would bind to the old unlinked inode and go stale
  — caching needs reopen-after-trim + invalidation, and `open`/`close` is noise
  at mail rate. The thing to revisit *at scale* is the whole-file read: switch
  `tail`/`peek` to targeted `pread` (read the tail, page back), which pairs with
  a longer-lived `Journal` that handles the trim-invalidation. Not now.
- [~] **Cursor → journal binding** — DECIDED (in progress): a durable value tag.
  A bare `Cursor` has no identity, so acking journal A with journal B's cursor is
  silently wrong (monotonic-advance does NOT catch a wrong-but-forward offset →
  mail loss/dup). A *pointer* is wrong (cursors outlive `Journal` instances and
  are persisted), so the binding is a **value tag** = a stable 64-bit FNV-1a hash
  of the journal name, carried in the Cursor, stamped by named journals
  (`consumerCursor` + `peek`/`dump`/`tail`), checked at `ack` (mismatch → refuse,
  fail-safe). Persisted: the token carries `offset:tag`; the cursor FILE stays
  8-byte (its path already binds it). **Deferred to P1:** the literal tag in the
  cursor-file bytes (collides with delivery's not-yet-retired 8-byte `readCursor`
  callers — fold in when those retire).

---

## Review still to walk (teaching, not a code change)

- `parseFrom` (forward read) and `tail()` (reverse-from-EOF + the torn-tail
  fallback) — where the both-ends framing earns its keep and the one subtle
  correctness bit lives.
