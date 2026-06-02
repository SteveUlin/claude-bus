# Broker hardening batch — 2 CRIT P0 + 2 resource-leak fixes

Status: **design** (elodin, 2026-06-02). Source: chronicler's blind audit (9
gaps), auri deduped + verified the CRITs file:line. sulin's load-balance
ruling: all 9 on one owner (elodin), CRITs-first, accept the serial-queue
depth. This batch is the first 4; **one broker-binary change → one graceful
restart** (which also carries `CLAUDE_BUS_CTX_WINDOW=1000000`, folded forward
from the GC deploy). Then P2 (auto-recovery), then the #5–#9 MED tail.

Owner: elodin. Design-doc-first because #1/#2 touch the RPC intake read path
(restart/connection semantics).

## Threat framing

Both CRITs are reachable **only from the local `broker.sock`** (mode `0600`,
owner-only). No untrusted client exists today, so this is **P0-hardening**, not
drop-everything — but both turn a single malformed/stalled local client into a
broker-wide outage (SIGSEGV or RPC-deafness), losing all in-memory state. The
broker is the fleet's single source of truth; its liveness is load-bearing.

## CRIT #1 — Unbounded JSON-parser recursion → SIGSEGV

**Where:** `json_min.cpp` `parseValue`/`parseArray`/`parseObject`;
entered from `rpc.cpp:409 json::parse(*line)` on every request.

**Bug:** `parseValue` (45) dispatches `{`→`parseObject` (213), `[`→`parseArray`
(187), each of which calls `parseValue` per element — mutual recursion with
**no depth tracking**. A ~1 MiB line of `[[[[…` recurses ~500K frames deep,
blows the 8 MB stack → **SIGSEGV**, every in-memory cursor/in-flight/registry
gone (the broker is the singleton).

**Fix:** a recursion-depth cap. `parseValue` is the single funnel every nested
container passes through, so guard there:

```cpp
// member
int depth_ = 0;
static constexpr int kMaxDepth = 256;  // legit RPC payloads are shallow (<10)

auto parseValue() -> std::expected<Value, std::string> {
  struct Guard { int& d; ~Guard(){ --d; } } g{++depth_ ? depth_ : depth_};
  if (depth_ > kMaxDepth)
    return std::unexpected{"max nesting depth exceeded"};
  skipWs();
  ...
}
```

(Cleaner: a small RAII `DepthGuard{depth_}` that `++` on construct / `--` on
destruct, declared before the cap check.) 256 is ~25× the deepest real payload
and ~2000× below the crash threshold. The cap converts a crash into a clean
`errorResponse` the intake thread already writes back (`rpc.cpp:413`).

**Test (unit, deterministic):** in `test_json.cpp`, `parse(std::string(100000,
'['))` returns an error (not a crash) and a 250-deep nest parses fine; a 300-
deep nest errors. No process death.

## CRIT #2 — `readLine` parks intake forever on a silent client

**Where:** `rpc.cpp:395` `::accept` → `:400 readLine(conn)` → `:447 ::read`.

**Bug:** `listen_fd_` is non-blocking (my #13 SOCK_NONBLOCK fix), but the
**accepted `conn` is blocking** (plain `::accept`, not `accept4`, no
`O_NONBLOCK`/timeout on the conn). A client that `connect`s, writes a partial
line (no `\n`), and never closes parks the **single intake thread** in `::read`
forever → broker goes **RPC-deaf** (delivery still ticks, masking it). Distinct
from #13, which fixed the *listen* fd; this is the *accepted* conn.

**Fix:** bound both directions of the conn with socket timeouts right after
`accept`:

```cpp
const int conn = ::accept(listen_fd_, nullptr, nullptr);
if (conn < 0) { ... }
struct timeval tv{.tv_sec = connTimeoutMs()/1000,
                  .tv_usec = (connTimeoutMs()%1000)*1000};
::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
::setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
```

- **Why both directions:** the error-write-back (`writeAll(conn, …)` on a
  readLine/parse error, `rpc.cpp:404/413`) is a *symmetric* wedge — a connected
  client that never reads parks `writeAll` on a full send buffer. `SO_SNDTIMEO`
  bounds it.
- **`readLine` EAGAIN branch:** on `SO_RCVTIMEO` expiry `::read` returns -1 with
  `errno ∈ {EAGAIN, EWOULDBLOCK}`. Add an explicit branch returning a clear
  `"read timeout"` error (today it falls through to the generic `strerror`
  path, which already closes the conn — so this is clarity, not correctness).
- **Timeout value:** default ~5 s (env `CLAUDE_BUS_CONN_TIMEOUT_MS`, mirroring
  the ack-timeout pattern). A legit `call()` connects, writes, half-closes
  within ms — 5 s is generous. `SO_RCVTIMEO` is **per-read**, so a slow-but-
  progressing large body keeps getting fresh windows; only a *stalled* client
  is cut. Multi-read bodies (>4 KiB) are unaffected.

**Test (itest):** a client that connects + writes `{"op":"info"` (no newline) +
holds the conn open → the broker answers a *subsequent* normal `bus broker
info` within ~timeout+ε (proves intake wasn't parked). Pre-fix: the second RPC
hangs. Drive with a raw socket from bash/python; assert on broker
responsiveness, not the stuck client.

## #3 — `inbox-ops` / `inbox-human` grow unbounded (retention hole)

**Where:** `delivery.cpp:1689-1691` (the `guaranteed` clamp).

**Bug:** D1/D2 retention is built, but `inbox-ops`/`inbox-human` are
`agent-inbox` (`guaranteed`), so `planTrim` clamps the cut to
`minConsumerCursor(tc.name)`. Nothing ever **drains** them (no ops/human pane),
so their consumer cursor sits at the header → `min_cursor == header` → trim can
never advance → unbounded growth. The broker is their sole perpetual producer
(escalations / delivery-failure notices).

**Fix:** the consumer-cursor clamp exists to protect *undelivered mail to a live
consumer*. The reserved-infra inboxes (`ops`, `human`) have **no live
consumer** — they're broker-produced sinks. Exempt them: treat them as
fire-and-forget for *retention* (size/age trim, newest-kept), i.e. drop them
from the `guaranteed` set for the clamp only:

```cpp
const bool reserved_sink =
    tc.name == "inbox-ops" || tc.name == "inbox-human";
const bool guaranteed = !reserved_sink &&
    (tc.kind == kKindAgentInbox || tc.kind == kKindTuiCommands);
```

This bounds them by the existing 8 MiB `CLAUDE_BUS_TOPIC_MAX_BYTES` cap while
keeping the newest escalations (the ones a human would actually read). The GC
reaper still *protects* these topics (reserved set) — we bound the log, never
reap the topic. Consistent with the `{human, ops}` reserved set in `broker gc`.

**Test:** unit on `planTrim` already covers clamp vs no-clamp
(`test_retention.cpp`); add a case asserting a reserved-sink name trims past a
stuck consumer cursor. (No new mechanism — just the predicate.)

## #4 — Per-agent maps never evict dead agents

**Where:** `delivery.cpp` — `token_scan_` (1496), `auto_clear_next_allowed_ms_`
(1049), `wake_next_allowed_ms_` (1403), `would_recover_next_log_ms_` (1173,
agent-keyed entries). All `operator[]`-inserted, never `.erase`d — unlike
`strand_alarmed_`/`mail_queued_since_ms_` (1346-47) and
`turn_stuck_alarmed_`/`tool_wedged_alarmed_` (947). P4 dynamic peers mint unique
names → these maps grow unbounded across spawn/despawn churn.

**Fix:** a single `forgetAgent(std::string_view name)` that erases the agent's
key from *all* per-agent maps, called when a pane disappears (GONE) — the same
trigger that already erases the alarm maps. One helper, called from the
pane-disappear path, keeps every per-agent map's lifetime tied to the agent.
Lower-risk than scattering `.erase` calls. Verify the GONE/pane-disappear
detection point and route the call there.

**Test:** unit-ish — after marking an agent GONE, assert its keys are absent
from the maps (may need a small test seam, or assert via an `inflight`-style
debug RPC; keep it light — this is a leak fix, not a correctness invariant).

## Deploy

All four are one broker-binary change → **one graceful restart** (the live
broker now has the SIGTERM-shutdown-fix, so SIGTERM, not SIGKILL). The restart
**must** set `CLAUDE_BUS_CTX_WINDOW=1000000` (auri's folded-forward loose-end —
the wrapper-less fallback window). Settle canonical first (D4 path under
`build.lock`), verify `bus broker info` `build_commit`, then relaunch via a
floating direct-zellij-child pane. Folding #3+#4 in does **not** change restart
timing — still one restart.

## Out of scope (next, in order)

- **P2** broker auto-recovery triage (signature→action→guard + OTP MaxR/MaxT) —
  see [[project_p2_auto_recovery]].
- **#5–#9** MED tail (writeCursor-fail dup, swallowed-append strands,
  fork-malloc deadlock, dispatchTui-sleep wedge, in-flight-wipe comment) —
  deferred behind P2.
