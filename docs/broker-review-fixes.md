# Broker review-fixes batch (chronicler review, 2026-06-05)

Status: **in progress** (elodin, task #1780774311766-elodin-097e). sulin
delegated the PROPOSE calls to auri; this ships the 4 review findings in priority
order. **kernel-0-0 is design-doc-first (auri reviews this approach before I
land it — it's the moat); delivery-0-0 / delivery-2-0 / rpc-1-0 ship directly
(build+ctest green, land each).** Owner: elodin.

## 1. kernel-0-0 (HIGH) — torn record on a short `::write` in `TopicLog::append`

### The bug

`topic_log.cpp` `append()` ends with a single unchecked-length write:

```cpp
const auto n = ::write(fd.get(), record.data(), record.size());
if (n != static_cast<ssize_t>(record.size())) {
  return std::unexpected{errFromErrno(errno, "append record")};
}
```

A **short write** (`0 < n < record.size()`) — ENOSPC mid-record, an EINTR signal,
a pipe/quota limit — has, under `O_APPEND`, **already appended the partial bytes**
to the log. The function returns an error, but the **torn record stays on disk**.
Its length prefix (`putU64` at offset 0 = the *full* record length) now overstates
the bytes actually present, so the next append lands right after the torn bytes and
`parseFrom` reads the full claimed length — consuming into the following record.
**Every record after the tear is misframed** → cursor byte-addressing collapses →
at-least-once + dedup break. This is a rule-#1 (kernel-triad) violation: the
append-only Log's framing is the foundation everything else rests on.

### The approach

**Guarantee all-or-nothing append: either the full record lands, or the log is
left exactly as it was.** Two mechanisms, combined:

1. **Loop the write** (handles EINTR + benign partials): retry on `EINTR`;
   on a partial (`n > 0`) advance the pointer and continue. Under `O_APPEND`
   each `::write` atomically targets the current EOF, so successive writes
   append the remainder contiguously — the record is completed across calls.
2. **`ftruncate`-back on a hard failure** (handles ENOSPC etc.): `fstat` the
   file size *before* the write; if the loop exits with bytes still unwritten
   (a non-EINTR error after a partial), `ftruncate(fd, size_before)` removes the
   partial bytes and returns the error. The log is unchanged → no torn record.

```cpp
struct stat st{};
if (::fstat(fd.get(), &st) != 0)
  return std::unexpected{errFromErrno(errno, "fstat topic log")};
const off_t size_before = st.st_size;

const std::byte* p = record.data();
std::size_t remaining = record.size();
while (remaining > 0) {
  const auto n = ::write(fd.get(), p, remaining);
  if (n < 0) { if (errno == EINTR) continue; break; }  // hard error → bail
  p += n;
  remaining -= static_cast<std::size_t>(n);
}
if (remaining > 0) {
  const int saved = errno;
  ::ftruncate(fd.get(), size_before);  // roll back the partial — no torn record
  return std::unexpected{errFromErrno(saved, "append record (rolled back)")};
}
```

### Why `ftruncate`-back is safe here (the load-bearing fact)

`ftruncate(fd, size_before)` is only correct if **no other writer appended
between our `fstat` and our `ftruncate`** — otherwise we'd truncate *their*
record. **Verified: topic logs are single-writer.** Every `TopicLog::append`
call site is in `delivery.cpp` / `broker.cpp` (the broker process); no CLI /
`sub_*` path appends directly (those go through the broker's enqueue RPC). The
broker is a single-threaded pselect tick loop — RPC handlers and the delivery
loop run on one thread. So there is exactly one writer, single-threaded:
`fstat → write-loop → ftruncate` is effectively atomic. (If a second writer is
ever introduced, this fix must move to a write-to-tmp + atomic-rename or a file
lock — flagged so the assumption is explicit.)

### Test plan

- **Unit (fault-injected short write):** the hard part is forcing a short write.
  Approach: a tiny seam — append into a path on a constrained sink, or factor the
  raw write behind an injectable functor in the test build — assert that after a
  simulated short write the file size is exactly `size_before` (rolled back) and
  `parseFrom` over the file yields the records that existed before (no torn tail).
  If the seam is too invasive, fall back to a direct `ftruncate`-back unit on a
  hand-built torn file + a `parseFrom`-stops-cleanly assertion.
- **Defense-in-depth (separate, noted):** `parseFrom` should also *stop cleanly*
  when the final record's claimed length exceeds the remaining bytes (treat a
  torn tail as EOF, never misframe). That hardens the read side even if a tear
  ever slips in. Proposed as a follow-on, not bundled into this fix.
- Existing topic_log + retention + dup-delivery itests stay green (the happy path
  — full write — is unchanged: the loop completes in one iteration).

## 2. delivery-0-0 — TTL/epoch evaluated before the in-flight guard (double-dispatch)

`dispatchAgentInbox` checks TTL (≈:570) and epoch (≈:583) and **advances the
cursor past** an expired/stale record *before* the in-flight guard (≈:599). A
record that is **in-flight AND TTL-elapsed** gets its cursor advanced past it
while still tracked in-flight → double-dispatch + a cursor that can't be
reconciled with the in-flight set. **Fix: hoist the in-flight guard to the TOP of
the per-record loop** (before TTL/epoch), in both `dispatchAgentInbox` **and**
`dispatchTuiCommands`. Ships directly. Test: the existing dup-delivery itest +
a case where an in-flight record's TTL elapses (assert no second dispatch, cursor
unchanged).

## 3. delivery-2-0 — payload disk-leak

`$STATE/payloads/<id>.body` (large bodies materialized on dispatch) is never
deleted → unbounded disk growth. **Fix: delete it on every terminal path** —
`onAck` / `forgetInflight` (acked or dropped), escalation, and SessionEnd inbox
release. Care: `bus msg body` reads the payload file and must stay side-effect-
free (no delete on read). Ships directly. Test: enqueue a large body, drive it to
ack, assert the payload file is gone; assert `bus msg body` does not delete.

## 4. rpc-1-0 (DOC-FIX only) — intake audit downgrade

Amend `broker-intake-decouple.md` §4 to record reality: the audit record the
intake path "would" write can't be written under the no-shared-state invariant,
so it **downgrades to a stderr log**. Doc only; no code.
