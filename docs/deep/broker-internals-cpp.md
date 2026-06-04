# Broker Internals & C++ Craft — A Deep Reference for claude-bus

> **FROZEN — pre-refactor archaeology (Phase-4 doc cleanup, 2026-06-03).**
> Documents the pre-shatter `delivery::Loop` architecture. Not maintained
> through the broker-seam refactor (`docs/broker-seam-redesign.md`); a lean set
> mapped to Log / Router / Transport / Readers is regenerated after Phase 2
> lands. Historical context, not current truth.

> Written 2026-05-28. The deep companion to `docs/modern-agent-techniques.md`,
> scoped to one thing: **the C++23 engineering of the broker itself.** Event
> loops, incremental parsing, event-sourced state reducers, lock semantics,
> JSON parsing, structured logging, and turning state-inference wedges into
> unit tests. Every section leads with the *principle* — the mechanism, the
> tradeoff, the design pressure — then drops to implementation depth with real
> code from our tree and from comparable systems (overstory's SQLite-WAL bus,
> claude-fleet's Rust ring buffer, simdjson, epoll/io_uring writeups). The
> "How this maps to claude-bus" section near the end is where the
> recommendations and the flaws-we-spotted live; the rest is the design space.

The broker (`src/broker.cpp`, `src/delivery.cpp`, `src/rpc.cpp`) is a
single-threaded C++23 daemon. One `pselect` loop interleaves an `AF_UNIX`
JSON-RPC server with a 250 ms delivery tick. The tick scans an append-only
event log (`events.jsonl`), folds it into per-agent lifecycle state, walks
typed topic logs, and dispatches records into live zellij panes through a
flock'd TTY write. State is files: binary topic logs, u64 cursor files,
per-message in-flight JSON, presence sentinels. There is no database, no
threads, no async runtime. That minimalism is a real strength — it is
trivially debuggable and crash-recoverable — and it is also where the sharp
edges hide. This document maps those edges.

The single most important framing for *this* topic: **the broker is an
event-sourced system that hasn't named itself one yet.** It already has an
append-only fact log, per-consumer cursors, idempotent record IDs, and a
boot-epoch fence. What it lacks is a *typed reducer* over those facts and a
*test harness* that replays canned event sequences. Almost every reliability
win below follows from completing that arc.

---

## Part 1 — The Event Loop

### 1.1 Principle: a reactor blocks on readiness; a poller burns time guessing

A daemon either *waits to be told* something is ready (reactor: `epoll`,
`io_uring`, `pselect`) or *wakes on a timer and checks* (poller). The reactor
trades a little setup complexity for two structural wins: **latency** (you act
the instant a thing is ready, not up to one poll-interval later) and **idle
cost** (a sleeping `epoll_wait` consumes zero CPU; a 250 ms poll loop wakes 4×
a second forever, each wake re-`stat`-ing files and re-reading registries).

The deeper pressure is **liveness coupling**. In a poll loop, the loop *body*
is a serial list of "and then check this." If any one check blocks, the whole
loop stalls — including unrelated work. A reactor decomposes the body into
independent handlers keyed off independent fds, so a slow handler can't starve
a fast one (unless you let it; see 1.4).

### 1.2 How claude-bus does it today: `pselect` + tick callback, with a scar

`src/rpc.cpp::Server::run` is a hybrid. It `pselect`s on the listen socket with
a timeout equal to the tick interval. Timeout → run the delivery tick.
Readable → accept and serve RPCs, then run the tick. The interesting part is
the **inner-loop budget cap** — and the comment on it is a postmortem:

```cpp
// History: without this cap, sustained 1-Hz polling from viewer
// panes (monitor + per-agent agent-bar) combined with slow
// per-RPC paneState calls (zellij dump-screen sometimes blocks
// for seconds) kept the inner loop indefinitely topped up. The
// broker stayed alive, RPCs returned, but the delivery tick
// never fired — records sat un-acked, in-flight retries never re-armed.
constexpr auto kInnerBudget = std::chrono::milliseconds{100};
```

This is the "broker delivery wedge" from sulin's memory, in the code. The root
cause is **the loop body is serial and one branch (RPC serving) can monopolize
it.** The budget cap is a correct mitigation, not a cure: it bounds the
starvation window to 100 ms, but the delivery tick still shares a thread with
synchronous `zellij dump-screen` subprocesses that "sometimes block for
seconds" (`src/pane.cpp` even wraps them in a 5 s SIGKILL timeout —
`waitWithTimeoutOrKill`). A single slow `dump-screen` inside a tick still
freezes RPC serving and every other agent's delivery for up to 5 seconds.

### 1.3 The design space: `pselect` → `epoll` → `io_uring`

| Mechanism | Wakes on | Idle CPU | Latency | Fd ceiling | Complexity | Fit for the broker |
|---|---|---|---|---|---|---|
| `sleep`/poll | timer only | burns | up to interval | n/a | trivial | what a v0 would do |
| **`pselect` + timeout (today)** | socket OR timer | low | up to 250 ms for delivery | ~1024 (FD_SETSIZE) | low | works; the wedge scar shows its limits |
| `epoll` + `timerfd` + `inotify` | *any* registered fd | ~zero | sub-ms | ~unbounded | medium | **the right next level** |
| `io_uring` | submitted ops complete | ~zero | sub-ms, batched | unbounded | high | premature here |

**Why epoll is the right ceiling and io_uring is not.** The consensus from the
2025/2026 literature is unambiguous for our shape. "For a single-threaded
daemon watching ~10 sockets with a few thousand requests per second, epoll is
preferable since the added complexity of io_uring is not worth it"
([Linux Kernel Internals](https://kernel-internals.org/io-uring/io-uring-vs-epoll/)).
io_uring's win — memory-mapped SQE/CQE ring buffers that let "thousands of I/O
operations [happen] with a single `io_uring_enter()` call"
([gocodeo](https://www.gocodeo.com/post/what-is-io-uring-high-performance-i-o-in-linux))
— only pays off "at over 50K concurrent connections [where] the per-event
syscall cost of epoll becomes measurable." The broker has a handful of fds and
a few RPCs per second. The crossover isn't close. Worse, io_uring is "faster
than epoll for ping-pong mode workloads, but **slower for streaming mode
workloads**" (axboe/liburing
[#189](https://github.com/axboe/liburing/issues/189),
[#536](https://github.com/axboe/liburing/issues/536)) — and tailing an
append-only log is streaming.

**What the epoll version looks like.** The reactor composes three fd types into
one `epoll_wait`:

- **`inotify`** watch on `$STATE/topics/` (and `events.jsonl`). Delivery wakes
  on an actual write instead of polling 4×/s. The iafisher note's key
  edge-trigger discipline applies: on a ready fd "you should loop over it
  calling `read` … until it returns `EAGAIN`/`EWOULDBLOCK`"
  ([iafisher](https://iafisher.com/notes/2025/10/epoll-io-uring)) so you don't
  miss coalesced events.
- **`timerfd`** for retry/ACK deadlines. Today `scanRetries()` walks the entire
  in-flight map every tick comparing `next_retry_at <= now`. A single `timerfd`
  armed to the *earliest* deadline (a min-heap of deadlines) fires exactly when
  something is due and never before — O(1) wakeups instead of O(in-flight) ×
  4/s.
- **The `AF_UNIX` listen socket**, exactly as today.

The structural payoff matches the wedge postmortem: **there is no monolithic
loop body to starve.** Each ready fd dispatches its own handler. The one thing
epoll does *not* fix for free is the blocking `dump-screen` subprocess — that
needs 1.4 regardless.

> **Skeptic's note.** Don't reach for io_uring's *file* ops to read the topic
> logs either. The logs are tiny and `pread` is already non-blocking enough.
> io_uring would buy nothing and cost a liburing dependency in a tree that
> currently links zero third-party libraries.

### 1.4 The real liveness bug: synchronous subprocesses on the loop thread

**Principle: never run unbounded-latency I/O on the thread that owes a
deadline.** `paneState()` and `dispatch::dispatchTui()` fork `zellij` and block
the broker thread until it returns or the 5 s SIGKILL fires. Every such call is
a 5 s hole the delivery loop can fall into. epoll doesn't help — the syscall is
*synchronous by construction*.

Three principled fixes, cheapest first:

1. **Cache pane state with a TTL.** `dispatchAgentInbox` calls `paneState(agent)`
   on every idle-gated record (`delivery.cpp:513`). The same dump is recomputed
   for the `state` RPC, the auto-clear scan, and each dispatch. A 200–500 ms
   per-agent cache collapses N forks/tick to ≤1, and is the single highest
   leverage / lowest effort change in this whole document.
2. **Move `dump-screen` off the loop thread.** A small thread pool (or a
   `popen` whose pipe fd is registered with epoll) turns the blocking call into
   a future the loop polls. The loop never blocks; it picks up the result on a
   later tick.
3. **Stop reading the TUI to infer state at all** — derive readiness from the
   event log (Part 3). The pane dump becomes a rare tiebreaker, not a
   per-dispatch syscall.

How others avoid this entirely: **overstory never reads a terminal.** Its
headless dispatcher (`src/agents/headless-mail-injector.ts`) polls a SQLite
mail table on a `setInterval` and, on unread mail, spawns one `claude --resume`
turn over a stdin pipe. There is no screen to scrape, so there is no
blocking-subprocess-on-the-loop problem — readiness is "did the prior turn's
process exit," not "what does the bottom row of the pane say." That's the
transport tradeoff the prior doc covered; here the point is narrower: **screen
scraping is what forces blocking subprocesses onto the loop thread.**

---

## Part 2 — Incremental Parsing & the Append-Log Reader

### 2.1 Principle: a tail reader must be offset-stable and partial-line-safe

Reading a growing append-only file correctly is subtle. Two invariants:

- **Never re-read consumed bytes.** Persist a byte offset; resume from it.
- **Never act on a torn record.** A writer may have flushed half a line when you
  read EOF. You must detect the partial tail and *not* advance the offset past
  it, so the next read re-sees it whole.

claude-bus gets this right, and the same pattern appears in three places —
which is itself a smell (it should be one reusable component). `scanEvents()`
(`delivery.cpp:231`):

```cpp
auto valid_pos = in.tellg();
while (std::getline(in, line)) {
  if (in.eof()) break;       // partial line, wait for the rest
  valid_pos = in.tellg();
  // ... parse + act ...
}
events_offset_ = static_cast<std::int64_t>(valid_pos);  // only past whole lines
```

The `if (in.eof()) break;` is the partial-line guard: `getline` sets eofbit
when it hits EOF without a terminating `\n`, so the half-line is dropped and the
offset stays before it. `maybeScanTokens()` (`delivery.cpp:959`) reimplements
the identical dance for transcript JSONL; the topic-log binary reader
(`topic_log.cpp::parseFrom`) does the binary analogue — `if (pos + rec_len >
buf.size()) break;` refuses a truncated record. **The pattern is correct three
times over; it should be one `TailReader` class.**

### 2.2 The binary topic log: a tidy framed format with one latent hazard

`topic_log.h` documents a clean v4 wire format: 64-byte file header
(`"BUS\0"` + u32 version), then length-prefixed records (u64 `record_len`
first, so the parser can skip a record it can't fully parse). `parseFrom`
bounds-checks every field read against `buf.size()` before dereferencing —
textbook defensive parsing. The version check in `readAll` rejects a
format-drifted log loudly (`"has format v{}, runtime expects v{}; wipe and
retry"`) instead of misparsing it.

**The latent hazard: `readAll` slurps the entire file on every `peek`.**

```cpp
auto TopicLog::peek(std::int64_t start_offset, std::size_t limit) const {
  auto buf = readAll(path_);            // reads the WHOLE file …
  return parseFrom(*buf, start_offset, limit);  // … then starts at start_offset
}
```

`readAll` does one `pread` of `st.st_size` bytes, then `parseFrom` seeks to
`start_offset` and parses forward. For a cursor near the tail of a long log,
this reads megabytes to return one 200-byte record. On a 250 ms tick that
peeks every agent-inbox topic, this is O(total log bytes) per tick. It works
because logs are small today, but it scales the wrong way, and `body` RPC is
worse — it `dump()`s *every topic's entire log* to resolve one msg_id
(`broker.cpp:571`). The fix is a `pread` at `start_offset` for a bounded window
(read 64 KiB, parse what's whole, read more only if `limit` not met).

### 2.3 The string-scan state reader: the brittle one

`agent_status.cpp::extractField` is the antithesis of `parseFrom`. It does a
substring search for `"key":"` and reads until the next quote, with a candid
comment:

```cpp
// Pull "key":"value" out of a JSONL line. No escape handling … A real
// parser would handle escapes; the log writer guarantees we never have them.
```

This is the regex-over-events brittleness the prior doc flagged. It's a *flat*
substring scan, so it can't distinguish `"agent":"bob"` at the top level from
an `"agent"` key nested inside a `payload` blob — it returns the first match.
`readAgents` then builds the whole lifecycle picture from these scans
(`agent_status.cpp:368`). Notably, the broker *already has* a real JSON parser
(`json_min`) and `delivery.cpp` already uses it correctly (`json::parse(line)`
→ `v->getOrString("event")`). **The state reader simply predates the parser and
never got migrated.** That's the cheapest correctness win in the tree:
`readAgents` should parse with `json_min` and read typed fields, deleting
`extractField` entirely.

### 2.4 The JSON parser: `json_min` vs simdjson vs nlohmann

**Principle: parse once, at the boundary, into typed values — never re-scan raw
bytes downstream.** The question is only *which* parser.

`json_min` (`json_min.cpp`) is a hand-rolled recursive-descent parser, ~360
lines, returning `std::expected<Value, std::string>`. It covers exactly the
schema the broker controls: null, bool, **int only (no floats)**, string,
array, object, with full `\uXXXX` → UTF-8 escape handling. It's clean,
dependency-free, and `std::expected`-based — idiomatic C++23. Its one real
limitation is **no float support**: `parseInt` reads digits with
`std::from_chars<int64_t>` and would reject `1.5e3`. Today nothing in the
schema is a float, but a transcript `usage` field or a future hook payload
could be, and it would silently fail to parse the *whole line*.

| Parser | Throughput | Deps | Floats | Style fit | When |
|---|---|---|---|---|---|
| **`json_min` (today)** | adequate (KB/s of tiny lines) | none | **no** | perfect — `std::expected`, flat `bus::` | keep for RPC + controlled schema |
| **simdjson** | 4× a production parser; NDJSON >3 GB/s; UTF-8 validation 13 GB/s ([simdjson](https://github.com/simdjson/simdjson)) | header-only | yes | good | *if* event/transcript volume ever dominates |
| nlohmann/json | slow, ergonomic | header-only | yes | heavy templates | not worth it here |

**The simdjson case, specifically.** simdjson's `ondemand` model + `parse_many`
is *built for exactly the broker's hottest path* — tailing NDJSON. `parse_many`
streams newline-delimited JSON and "exceed[s] 3 GB/s." Its On-Demand parser is
lazy: `parser.iterate(json)` "defers processing until values are actually
accessed," so reading just `event` and `agent` from a line skips parsing the
fat `payload` blob entirely. The catch is the **padding requirement**: simdjson
needs `SIMDJSON_PADDING` bytes past the buffer end and a `padded_string`, which
complicates the partial-line tail logic. **Verdict: not yet.** The broker
parses kilobytes, not gigabytes; `json_min` is the right tool until profiling
says otherwise. The principle to *adopt now* from simdjson is "parse lazily,
read only the fields you need" — which `json_min`'s `getOrString` already
allows; the migration of `readAgents` (2.3) realizes it.

---

## Part 3 — Event-Sourced State: the typed reducer

### 3.1 Principle: state should be a pure fold over an immutable fact log

Event sourcing says: don't store *state*, store the *events that produced it*,
and derive state by folding a reducer `(State, Event) → State` over the log
from a stored cursor. The wins are exactly the ones the broker needs:

- **Replayable.** Feed the same events, get the same state. A production wedge
  becomes a canned event sequence you replay in a test.
- **Testable in isolation.** The reducer is a pure function — no files, no
  zellij, no time except what the event carries.
- **Impossible-state-proof.** If transitions are a real state machine, illegal
  `(state, event)` pairs are caught at the transition, not discovered as
  corruption later.

### 3.2 Where claude-bus already is (further than the prior doc credited)

The prior doc said "replace regex-over-events with a typed reducer." Reading the
code, `agent_status.{h,cpp}` has **already done the hard modeling half** and
just hasn't finished the mechanical half. `computeAxes` (`agent_status.cpp:250`)
is a near-pure function from `(AgentInfo, unread, now_ms, pane_exists, pane*)`
to four **orthogonal axes** — `ProcessAxis`, `TurnAxis`, `MailAxis`, `TuiAxis`
— with `computeState` as a compatibility projection down to the flat `State`
enum. This is *good* design: orthogonal axes resist the combinatorial explosion
of a single mega-enum, and the header comments are genuinely instructive about
*why* each axis exists.

But it is **not yet a reducer.** Three gaps:

1. **It folds over only the *last* event, not the stream.** `readAgents` keeps
   only `info.last` per agent (`agent_status.cpp:396` — every field is
   overwritten each line). So `computeAxes` is `f(last_event)`, not
   `fold(events)`. This is precisely why the "mid-stream silent dropped turn"
   is invisible: the state derives from the latest event in isolation, with no
   memory that a tool call was *planned but never emitted*. A true reducer
   carrying a small accumulator (e.g. "saw `PreToolUse`, awaiting matching
   `PostToolUse`") would catch the gap as a state, not a mystery.
2. **It's time-coupled, not event-coupled.** `TurnAxis::Stuck` is `age_s > 5*60`
   (`agent_status.cpp:309`); `ProcessAxis::Stuck` is `age_s > 30`. Wall-clock
   thresholds make the function *impure* (depends on `now`) and untestable
   without mocking time. A reducer would model "stuck" as "no terminal event
   observed since the work-start event" and let a *separate* timer fd decide
   *when* to escalate — separating the fact (no Stop yet) from the policy (how
   long to wait).
3. **No cursor for state.** Topics have per-consumer cursors; state derivation
   re-reads `events.jsonl` from scratch every call (`readAgents` opens, reads to
   EOF, rebuilds the whole map). The broker's *own* state view should be a
   cursored consumer of the event log like everything else.

### 3.3 How a comparable system does correlation: overstory's tool_start/tool_end

overstory's event store (`src/events/store.ts`) shows the correlation pattern
that catches dropped turns. Events are *typed rows*, and a prepared statement
correlates a `tool_end` back to its open `tool_start`:

```sql
SELECT id, created_at FROM events
WHERE agent_name = $agent_name
  AND tool_name = $tool_name
  AND event_type = 'tool_start'
-- (the most recent tool_start with no tool_duration_ms set = still open)
```

That "most recent start with no matching end" *is* an in-flight tool call
tracked structurally. A `PreToolUse` with no `PostToolUse` after N is a concrete,
queryable fact — not something you eyeball in a pane dump. claude-bus has the
raw events (`PreToolUse`/`PostToolUse` both carry `tool_name`); it just discards
all but the last. A reducer that pairs them gives the dropped-turn detector for
free.

### 3.4 The shape of the reducer for claude-bus

```cpp
// Pure. No I/O, no clock except event.ts. Table-testable.
struct AgentAccumulator {           // the folded state, per agent
  ProcessAxis process{ProcessAxis::New};
  TurnAxis    turn{TurnAxis::None};
  std::optional<std::string> open_tool;   // PreToolUse seen, PostToolUse pending
  std::int64_t last_work_ts{0};           // when current turn's work began
  // ... no `now`; staleness is decided by the caller's timer fd ...
};
auto Reduce(AgentAccumulator, const Event&) -> AgentAccumulator;  // total
```

Make it **total**: every `(accumulator, event)` maps somewhere, even to
"unchanged." Then the test is a table: a `vector<Event>` in, an expected
`AgentAccumulator` out. The "mid-stream dropped turn" becomes:

```cpp
// Reproduces the wedge: PreToolUse with no PostToolUse, then a UserPromptSubmit
// that completed text but dropped the planned tool call.
auto acc = fold({UserPromptSubmit{}, PreToolUse{"Bash"} /* no PostToolUse */});
EXPECT_EQ(acc.open_tool, "Bash");   // would have been silently lost before
```

That is the prior doc's "turns a production mystery into a failing unit test,"
made concrete against *our* enum names.

---

## Part 4 — Locks, Concurrency, and Crash-Safety

### 4.1 Principle: `flock` is advisory and tied to the open-file-description

`flock(2)` enforces nothing against a process that doesn't *ask* for the lock,
and the lock releases on the last close of *any* fd duplicated from the locked
open. So a `flock`-based mutual exclusion is only as good as the invariant
"*every* writer takes the lock" — which the kernel will never enforce for you.

claude-bus uses `flock` in three correct-but-load-bearing ways:

1. **Singleton broker** (`broker.cpp:185`). `flock(pidfd, LOCK_EX | LOCK_NB)`
   on `broker.pid`. The comment documents *why* this replaced an earlier
   `O_EXCL + kill(pid,0)` scheme: a rebuilt binary left a running process with a
   deleted exe and a reset pid file, and two brokers started. flock "survives
   unlink, dies with the process, and serializes concurrent starts atomically."
   This is the *right* primitive for singleton-ness and the reasoning is sound.
2. **Per-pane TTY write** (`delivery.cpp::deliverInline`, `dispatch.cpp::FlockGuard`).
   A `$STATE/tui-locks/<agent>.lock` serializes byte writes so two senders don't
   interleave keystrokes into one pane. Correct — *as long as* every writer
   flocks. `bus pane-send` (the raw lever) deliberately does not, which is the
   documented escape hatch, but it's exactly the "writer that doesn't take the
   lock" the principle warns about. The invariant lives in prose, not code.
3. **The socket inode guard** (`rpc.cpp:~50`, `~Server`). Not a flock but the
   same *class* of bug solved well: the destructor only `unlink`s the socket if
   the on-disk dev+inode still match what *this* server bound — so a shutting-
   down broker A can't delete successor broker B's freshly-bound socket. This is
   careful, correct systems code.

### 4.2 The atomic-write discipline: tmp + rename, done consistently

The broker uses `write-to-tmp + rename` for every state file that must never be
seen half-written: cursors (`topic_log.cpp::writeCursor`), in-flight records
(`delivery.cpp::writeInflight`), the epoch file (`broker.cpp::writeEpoch`), and
the token-scan status (`delivery.cpp:1002`). `rename(2)` on the same filesystem
is atomic, so a reader sees either the old file or the new, never a torn one.
This is consistent and correct. **One gap:** `TopicLog::append` does a bare
`::write(fd, record, len)` with `O_APPEND` and no `fsync`. For a single-writer
append that's fine for *ordering* (O_APPEND is atomic per write on local fs),
but a power loss between `write` and the page-cache flush loses the tail record
silently. For an *audit* bus that's probably acceptable; if not, an `fdatasync`
after append (or periodic) is the knob. Worth a documented decision either way.

### 4.3 The boot-epoch fence: a genuinely clever crash-recovery primitive

`broker.cpp` bumps a persisted u64 epoch on every boot and stamps it into each
record's `correlation` field (`delivery.h::stampEpoch` repurposes the unused
RPC-pairing bytes). On dispatch, a record whose epoch ≠ the running broker's is
*quarantined*: escalated to `audit` + `inbox-ops`, cursor advanced, never
delivered (`delivery.cpp:483`). This solves a real problem elegantly — it lets
the broker keep durable topic logs across restarts (for audit/replay) **without
re-delivering yesterday's mail as new.** The escalate-self-stamping subtlety is
documented and correct (`delivery.cpp:669`: stamp the broker's own audit
emissions with the current epoch, or the next tick quarantines them and you get
an infinite escalation loop). This is the kind of detail that only shows up
after a real incident; it's well-handled.

### 4.4 The comparison: SQLite-WAL gives this for free, at a dependency cost

overstory and claude-fleet both put coordination state in SQLite WAL:

```ts
db.exec("PRAGMA journal_mode = WAL");      // concurrent readers + 1 writer
db.exec("PRAGMA synchronous = NORMAL");    // crash-safe, not fsync-per-write
db.exec("PRAGMA busy_timeout = 5000");     // retry on lock contention
```

(both `src/mail/store.ts` and `src/events/store.ts`). WAL buys: ACID, crash-safe
writes, atomic compare-and-swap for task claiming (`UPDATE … WHERE owner IS
NULL` → check `rows_affected==1`), indexed queries (overstory indexes
`events(agent_name, created_at)`, `events(tool_name, agent_name)` — the
correlation query is an index seek, not a scan), and *schema migration* (their
`migrateSchema` recreates the table when CHECK constraints change — versus
claude-bus's "wipe and retry" on format drift).

**But the prior doc's verdict holds and the code confirms why.** The broker is
*single-writer* — every producer goes through the broker via RPC
(`topic_log.h`: "The broker is the only writer to topic logs … so
concurrent-writer interleave isn't a risk"). SQLite's headline feature
(concurrent multi-writer safety) is *unused* in a single-writer design. What
the broker would actually gain from SQLite is **indexed lookup** (the `body`
RPC's all-topic scan, 2.2) and **crash-safe cursors/in-flight in one file**
instead of a directory of tiny JSONs. Those are real but modest, and they cost a
SQLite dependency in a zero-dependency tree plus the loss of `grep`-able,
append-only audit logs. **Net: keep append-log for the immutable event stream;
SQLite earns its place only if `body`/`drop` lookups or in-flight bookkeeping
become a profiled bottleneck.** The ring-buffer alternative (claude-fleet's
`crates/ringbus`, a per-topic `VecDeque` capped at 10k with `pop_front` on
overflow) is the *opposite* tradeoff — pure in-memory, no durability, drops
oldest on overflow. That's a fine model for ephemeral pub/sub but wrong for a
bus whose whole value is durable, replayable, attachable agents.

---

## Part 5 — Structured Logging & Observability of the Broker Itself

**Principle: the broker decides things; its decisions should be replayable
facts, not prose.** Today `broker.cpp::logEvent` writes a semi-structured line
to `broker.log`: `ISO8601 TAG key=val key=val`. It's greppable and the tags
(`START`, `DEFER`, `DROP`, `STOP`, `WARN`) are useful, but it's a *different*
format from the agent `events.jsonl` it consumes, so you can't replay broker
decisions through the same tooling.

The cheap upgrade: emit `broker.log` as **JSONL with the same envelope as
`events.jsonl`** (`ts`, `level`, plus `decision`, `agent`, `msg_id`, `reason`).
Then a single reader tails both, and "why did the broker quarantine that
record" is a query, not an archaeology dig. This costs ~20 lines (reuse
`json::serialize`) and makes the broker a first-class citizen of its own
observability story — the "cross-agent view scrollback can't give you" from the
project's own CLAUDE.md.

The decisions worth logging structurally, because each was a past incident:
epoch quarantine, defer-on-blocking-op, retry-exhaustion escalation, auto-clear
fires, and delivery-tick skips. Several already log; the win is one schema.

---

## Part 6 — Build & Test Ergonomics: the missing half

**Principle: a pure function with no tests is a liability waiting to regress; a
pure function *with* a table test is a spec.** The single biggest gap in the
broker's engineering is not in `src/` — it's the absence of unit tests for the
parts that are *already* pure and testable.

Current state (`CMakeLists.txt`, `tests/`): CMake builds one `bus` binary +ws a
`bus_agent_status` static lib. Tests are **shell integration tests**
(`tests/bus-itest.sh`, `mvp-test.sh`) that exercise the CLI end-to-end against
an isolated broker. Valuable for wiring, useless for the logic that wedges. The
state reducer, the topic-log parser, the JSON parser, the tail-reader
partial-line guard, the epoch stamp/extract — all pure, all currently untested.

**The unlock is mechanical, because the design is already right:**

- `computeAxes` takes plain structs and returns plain enums. A table test —
  `vector<canned events> → expected axes` — needs no zellij, no files, no
  broker. (Once 3.2's fold lands, it needs no clock either.)
- `topic::parseFrom` takes a `span<const byte>` and returns `vector<Message>`.
  Hand it a hand-built byte buffer (including a *truncated* tail) and assert it
  stops at the last whole record. That's the binary analogue of the
  partial-line guard, and it's the kind of thing that breaks silently on a
  format bump.
- `json::parse` round-trips: `parse(serialize(v)) == v`, plus malformed-input
  cases (the float gap from 2.4 becomes an explicit `EXPECT` that documents the
  limitation).

CMake makes this trivial: add a `bus_core` static lib (the pure pieces:
`json_min`, `topic_log`, `agent_status`, a future `reducer`), link the `bus`
binary against it, and add a `bus_tests` executable linking the same lib. No new
dependency is even required — a 30-line assertion harness in the existing
`bus::` style suffices, or pull in a header-only framework (doctest) under
`nix develop`. **This is the change that turns every other recommendation in
this doc from "trust the refactor" into "the test stays green."**

---

## Part 7 — How This Maps to claude-bus

Ranked by payoff ÷ effort. The unifying theme: **the broker is 80% of an
event-sourced system; finish the typed-reducer + test arc, and harden the
loop-thread liveness, before reaching for new substrates.**

### Recommendations

| # | Recommendation | Principle | Effort | Payoff |
|---|---|---|---|---|
| 1 | **Cache `paneState` per-agent with a 200–500 ms TTL** | Unbounded-latency I/O must not run on the deadline-owning thread; collapse N forks/tick to ≤1 | **Low** | **High** |
| 2 | **Migrate `readAgents`/state derivation off `extractField` onto `json_min`** | Parse once into typed values; never substring-scan JSON (the parser already exists in-tree) | **Low** | **High** |
| 3 | **Add a `bus_core` lib + unit tests for the pure pieces** (`computeAxes`, `parseFrom`, `json::parse`, tail guard) | A pure function with a table test is a spec; turns wedges into red tests | **Low–Med** | **Very High** |
| 4 | **Turn `computeAxes` into a true fold** carrying a small accumulator (open-tool, work-start), decoupled from wall-clock | State = pure fold over the fact stream; catches the mid-stream dropped turn structurally | **Med** | **High** |
| 5 | **Extract one `TailReader`** for the offset+partial-line dance reused in `scanEvents`/`maybeScanTokens`/`parseFrom` | One correct implementation beats three copies that can drift | **Low** | **Med** |
| 6 | **Bound `TopicLog::peek` to a windowed `pread`** instead of slurping the whole file; index or window the `body`/`drop` all-topic scan | Read work should be O(records returned), not O(log size) | **Med** | **Med** |
| 7 | **epoll + `inotify` + `timerfd`** replacing `pselect`+250 ms | Reactor: sub-ms delivery, ~0 idle CPU, no monolithic loop body to starve | **Med** | **Med** (do *after* 1, which removes the actual stall) |
| 8 | **`broker.log` as JSONL** with the `events.jsonl` envelope | Broker decisions should be replayable facts; one reader for both streams | **Low** | **Med** |
| 9 | **Document the `fsync`/durability decision** for `TopicLog::append` | Make the crash-loss-window an explicit choice, not an accident | **Low** | **Low** |

### Flaws spotted in the current code (file-referenced)

- **`rpc.cpp` loop-body starvation (mitigated, not cured).** The 100 ms inner
  budget bounds RPC starvation, but a single blocking `zellij dump-screen`
  inside a delivery tick can still freeze the whole loop for up to 5 s
  (`pane.cpp::kDefaultSubprocessTimeout`). Rec #1 attacks the cause.
- **`agent_status.cpp::extractField`** does flat substring JSON scanning with no
  nesting awareness; a `payload`-nested `"agent"`/`"event"` key could shadow the
  top-level one. The parser to fix it (`json_min`) already ships and is used
  correctly in `delivery.cpp` — the state reader just never migrated. (Rec #2.)
- **State derives from `info.last` only** (`agent_status.cpp:396` overwrites
  every field per line). It's `f(last_event)`, not `fold(events)`, which is
  exactly why the mid-stream dropped turn is invisible to the state machine.
  (Rec #4.)
- **Wall-clock thresholds make state impure** (`agent_status.cpp:279` `age_s>30`,
  `:309` `age_s>5*60`). State can't be table-tested without mocking time; the
  fact (no terminal event) should be separated from the policy (how long to
  wait). (Rec #4.)
- **`TopicLog::peek` slurps the entire file** via `readAll` then seeks
  (`topic_log.cpp:306`); `body` RPC `dump()`s every topic's whole log to find
  one id (`broker.cpp:571`). O(log size) per call. (Rec #6.)
- **`json_min` silently rejects floats** (`parseInt` only; `json_min.cpp:112`).
  Fine for today's schema, a latent whole-line parse failure if any future
  payload field is fractional. Document or extend. (2.4.)
- **No unit tests for any pure logic** — the highest-leverage absence, because
  the design is already test-shaped. (Rec #3.)
- **`flock` invariant lives in prose** — `bus pane-send` is a documented
  non-locking writer; nothing in code stops a future caller from interleaving.
  Acceptable as an escape hatch, but worth a louder comment at the `pane-send`
  definition. (4.1.)

### What to deliberately NOT do

- **Don't adopt io_uring.** The fd count and request rate are orders of
  magnitude below the crossover; it's slower for streaming workloads and adds a
  liburing dependency to a zero-dependency tree.
- **Don't move topic logs to SQLite.** Single-writer design makes WAL's headline
  feature dead weight; append-only files are crash-safe, replayable, and
  greppable — exactly what an audit bus wants. Consider SQLite *only* for
  mutable lookup state (in-flight, cursors) *if* profiling flags it.
- **Don't replace `json_min` with simdjson** until the event/transcript parse
  volume is profiled as hot. Adopt its *principle* (parse lazily, read only
  needed fields) now via the `readAgents` migration; adopt the *library* later
  if ever.
- **Don't ship the epoll rewrite before Rec #1.** The reactor is elegant but the
  *actual* stall is the blocking subprocess, which epoll doesn't fix. Sequence
  correctness (cache + reducer + tests) before the loop rewrite.

---

## Sources

**claude-bus source (this tree)**
- `src/broker.cpp`, `src/delivery.{cpp,h}`, `src/rpc.cpp`, `src/pane.cpp`,
  `src/agent_status.{cpp,h}`, `src/json_min.{cpp,h}`, `src/topic_log.{cpp,h}`,
  `src/dispatch.cpp`, `CMakeLists.txt`, `tests/bus-itest.sh`

**Comparable implementations (read at source)**
- [jayminwest/overstory](https://github.com/jayminwest/overstory) — SQLite-WAL
  mail + event stores (`src/mail/store.ts`, `src/events/store.ts`: WAL pragmas,
  prepared statements, schema migration, tool_start/tool_end correlation),
  headless stream-json dispatcher (`src/agents/headless-mail-injector.ts`:
  in-flight short-circuit, mark-read-after-success ACK), mail-poll detector
  (`src/agents/mail-poll-detect.ts`)
- [sethdford/claude-fleet](https://github.com/sethdford/claude-fleet) — Rust
  ring buffer (`crates/ringbus/src/lib.rs`: per-topic `VecDeque`, 10k cap,
  `pop_front` eviction, priority + `read_by` tracking)
- [mixpeek/amux](https://github.com/mixpeek/amux) — SQLite CAS task claiming,
  self-healing watchdog

**C++ / systems craft**
- [Notes on epoll and io_uring — iafisher](https://iafisher.com/notes/2025/10/epoll-io-uring)
- [io_uring vs epoll — Linux Kernel Internals](https://kernel-internals.org/io-uring/io-uring-vs-epoll/)
- [What Is io_uring? — gocodeo](https://www.gocodeo.com/post/what-is-io-uring-high-performance-i-o-in-linux)
- [io_uring slower than epoll — axboe/liburing#189](https://github.com/axboe/liburing/issues/189),
  [#536](https://github.com/axboe/liburing/issues/536)
- [simdjson](https://github.com/simdjson/simdjson) — On-Demand parsing,
  `parse_many` NDJSON, padding, throughput (>3 GB/s NDJSON, 13 GB/s UTF-8)

**Event sourcing**
- [Event sourcing with SQLite](https://www.sqliteforum.com/p/event-sourcing-with-sqlite)
- [SQL event store with dedup & ordering — mattbishop](https://github.com/mattbishop/sql-event-store)
</content>
</invoke>
