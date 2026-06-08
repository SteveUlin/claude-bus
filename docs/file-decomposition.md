# File Decomposition — extract real modules, not scattered bodies

Companion to [structural-review.md](structural-review.md). That doc hardens
*API shape*; this one attacks *file size* — but only through **true logical
separation**. Walk it by item number ("do M1"); each has a status checkbox.

## The principle (the one that governs every item)

**The unit of decomposition is the abstraction, not the file.** A new `.cpp`
earns its place *only* by publishing a new `.h` — a minimal interface that other
code programs against. Splitting one class's method bodies across several `.cpp`
files that share its existing header is **not** separation; it's the same
god-object with extra `#include`s, and it's explicitly rejected.

Corollary: **the highest-value cuts also delete duplication.** When the same
logic is hand-rolled in two large files, that duplication *is* the evidence a
module is missing. Extract the module, make both files clients. The line-count
drop is a side effect; the real win is one typed boundary replacing N copies.

So the count that matters here is **new headers** (new boundaries), not new
`.cpp` files.

---

## Cross-file modules — extract these first (each kills live duplication)

### M1 — `process.{h,cpp}`: child-process spawn + capture + timeout-kill

**New interface.** **Kills 2 copies.** **Risk:** LOW.

**Current:** the exact same "fork/exec a child, capture stdout, kill on
timeout" machinery exists **twice**:
- `pane.cpp`: `waitWithTimeoutOrKill`, `slurpFdWithDeadline`, `runCapture`,
  `runSilent` (~140 lines, anon-ns).
- `sub_lifecycle.cpp`: `runSync`, `runCaptureLocal` (~70 lines, anon-ns).

Both drive `zellij action …` / `pgrep` subprocesses. Two implementations of one
idea, neither testable.

**Proposed:** `src/process.h` declares a tiny interface —
`runCapture(std::span<const char* const> argv, std::chrono::milliseconds timeout)
-> Result<Captured>` (where `Captured{int rc; std::string out;}`) and
`runSilent(argv, timeout) -> bool`. `process.cpp` holds the one fork/exec/poll
implementation. `pane.cpp` and `sub_lifecycle.cpp` `#include "process.h"` and
delete their copies. Unit-testable against `/bin/echo` etc.

**Blast:** ~210 duplicated lines → ~100 in one TU + two `#include`s. New
`bus_process` lib (or fold into `bus_core`). Call sites are already
`runCapture(...)`-shaped, so they barely change.

**Status:** [x] LANDED — `bus::process`, main `5c462d4f`. Behavior delta:
lifecycle `new-tab`/`close-tab` moved off the old inherited-stdio, unbounded
`runSync` onto `runSilent` (stdio → /dev/null, 5 s timeout-kill).

---

### M2 — `peer_registry.{h,cpp}`: the dynamic-peers registry as a typed module

**New interface.** **De-dups across `sub_lifecycle.cpp` + `broker.cpp`.**
**Risk:** LOW.

**Current:** the `$STATE/dynamic-peers/` registry (one file per peer, atomic
add/remove) is open-coded as raw `stateRoot() + "/dynamic-peers"` + `fs` calls
in `sub_lifecycle.cpp` three times (`subSpawn` add, `subRestorePeers` list,
`subDespawn` remove) **and** in `broker.cpp`'s GC liveness scan. No type owns
the layout; a path change breaks four sites silently.

**Proposed:** `src/peer_registry.h` — `class PeerRegistry` built from the state
root, owning `add(name, meta)`, `remove(name) -> bool`, `list() ->
std::vector<Peer>`. The directory layout lives behind it (the same move J1's
`CursorStore` made for cursor files). `sub_lifecycle.cpp` and `broker.cpp`
program against it.

**Blast:** 4 open-coded sites → method calls. Folds in C4's `dynamicPeersPath()`
concern — the registry owns the path, so C4 doesn't need an accessor for it.

**Status:** [x] LANDED — `bus::PeerRegistry` + `struct Peer` in `bus_core`,
`tests/unit/test_peer_registry.cpp` (4 cases). All 4 sites (3 in sub_lifecycle,
broker GC) now program against `add`/`remove`/`list`. Minor: `restore-peers` on
an empty-but-present dir now prints "no dynamic peers registered" (was a 0/0
line) — `list()` collapses absent and empty.

---

### M3 — `time_util.h` (C2) extended with the ISO helpers

**New interface.** **Extends a planned extraction.** **Risk:** LOW.

**Current:** C2 already proposes `time_util.h` for `nowMs()` / `systemBootMs()`
(removing `journal.cpp`'s private copy). Separately, `sub_log.cpp` hand-rolls a
cluster of ISO-8601 helpers — `durationToMs`, `expandToIso`, `shiftIso`,
`thresholdIso`, `formatIso` — pure time-format functions with no logging
content.

**Proposed:** land C2's `time_util.h`, and move `sub_log.cpp`'s ISO helpers into
it (or a sibling `time_fmt.h`). `sub_log.cpp` keeps only its rendering.

**Blast:** absorbs C2; ~5 helpers relocate. Pure, testable.

**Status:** [ ] proposed (extends C2)

---

## Per-file modules — one real sub-module out, a thinner host left behind

### M4 — `pane_parse.{h,cpp}`: pure zellij-dump parser (becomes testable)

**New interface.** **Risk:** LOW–MED.

**Current:** `pane.cpp` tangles a pure parser into its IO layer (all anon-ns):
`trim`, `cutAtDivider`, `walkAnsi`, `stripAnsi`, `splitLines`, `isDividerLine`,
`findInputLine`, `detectMode`, `detectBypass`, `extractAnsiLine`, `parseInput` +
`InputParts` (~250 lines). It's string-in / struct-out — no syscalls — yet it's
**not unit-testable** because it's anon-ns inside the IO TU. This is the logic
most exposed to zellij output-format drift.

**Proposed:** `src/pane_parse.h` exposes the parser in `namespace bus::pane_parse`
— `detectMode`, `detectBypass`, `parseInput`, `stripAnsi`, … `pane.cpp` keeps the
actuator surface (`paneId`, `sendToPane`, `sendKey`, `sendToPaneSafe`,
`paneState`, caches) and becomes a client of `pane_parse` + M1. New
`bus_pane_parse` lib so `tests/unit/test_pane_parse.cpp` links the parser
without the subprocess code — the same no-authority link boundary the Readers
split uses.

**Blast:** ~250 lines move; `pane.h` public surface untouched.

**Status:** [x] LANDED — `bus::pane_parse` (+ `bus_pane_parse` lib),
`tests/unit/test_pane_parse.cpp` (10 cases). Verbatim move, zero behavior change.

---

### M5 — `context_stats.{h,cpp}`: lift the transcript-token reader out of `sub_monitor`

**New interface.** **Risk:** LOW–MED.

**Current:** `sub_monitor.cpp` buries `CtxStats` + `contextStatsFor` (~60 lines)
— it reads an agent's transcript JSONL and derives context-window usage. That's
a pure **observability reader** (Readers family), not a monitor-rendering
concern; any other viewer that wants CTX% re-derives it.

**Proposed:** `src/context_stats.h` — `struct CtxStats` + `contextStatsFor(agent)
-> CtxStats`, in `bus_readers` (its natural home: derives over files, no
authority). `sub_monitor.cpp` keeps `render()` and the small display formatters.

**Blast:** ~60 lines move into `bus_readers`; `sub_monitor.cpp` gains an include.
The rest of the viewer (one cohesive `render`) stays — correctly.

**Status:** [x] LANDED — `bus::CtxStats` + `contextStatsFor(state_root, agent)`
in `bus_readers` (state root INJECTED, Readers convention), display formatters
(`formatCtx`/`ctxColorFor`/`formatModel`) stay in `sub_monitor`.
`tests/unit/test_context_stats.cpp` (4 cases). Follow-up: `scanIntAfter` moved
with it but its string sibling (`extractStr` in `sub_monitor`) is now a private
copy in `context_stats.cpp` — these two flat-JSON scanners want a shared
`json_scan` util (the float fields block `json::parse`); noted, not built.

---

### M6 — `rpc_client.{h,cpp}`: split the client role from the server

**New interface.** **Risk:** LOW. **Value:** MED — optional.

**Current:** `rpc.cpp` holds two genuinely different roles: the **server**
(`Server`, `CmdQueue`, bind/accept/dispatch/run) and the **client** (`call`,
`readLine`, `writeAll` — talk to a broker over the socket). They never call each
other. This is a real role boundary, not body-moving.

**Proposed:** `src/rpc_client.h`/`.cpp` for the client; `rpc.cpp` keeps the
server + queue. `rpc.h` either splits to match or keeps both declarations.

**Blast:** the `call` site set updates includes only. Low payoff — schedule if
`rpc.cpp` keeps growing.

**Status:** [ ] proposed

---

## The elephants — true separation *is* the existing design

`delivery.cpp` (1526) and `broker.cpp` (1177) are big because of real
god-objects (`delivery::Loop`, the `runBroker` closure). The honest fix is **not**
a file shuffle — it's the responsibility split already designed in
[broker-seam-redesign.md](broker-seam-redesign.md): **Log** (= `bus::Journal`,
landed), **Router**, **Readers**, **Policy**, **Daemon** — each a real class
with its own `.h`/`.cpp` and a minimal interface. That doc is the authority for
this elephant; defer to its sequencing, don't re-plan it here.

**⚠ Transport is NOT a cut — settled at the gate.** An earlier draft of this
doc listed "Transport first." That was wrong: broker-seam-redesign **§6 OUTCOME**
(2026-06-04) examined it and the collapse-to-if tripwire fired. The two-arm fork
— `isOffTty()` selector + `deliverInline`→`sendToPaneSafe` (TTY arm) + "don't
push, the drain RPC pulls" (off-TTY arm) + `maybeWakeIdleOffTty` (doorbell) — is
**already factored** in `delivery.cpp`. A `Transport` class would be *ceremony
over an existing fork* — exactly the body-scattering this doc's principle
rejects. The seam's value was delivered as **documentation**
([broker-spec §"Delivery model"](broker-spec.md)), and that doc is accurate
against the current code. Do not extract it.

**The real `delivery.cpp` cuts** are the two *endorsed* seam steps — **trim→Log**
(retention math behind the Log boundary so offsets stop shifting mid-dispatch)
and **scanEvents→Router.onAck** (split the parse-vs-ack weld). Both are
behaviorally neutral but **touch the at-least-once / ack / offset kernel**, are
gated on P2, and carry the design's "never big-bang; broker delivers mail at
every commit" rule. That's supervised kernel work — schedule it deliberately,
not as a solo autonomous pass.

The earlier "mechanical TU split of `Loop` across `delivery_*.cpp`" idea is
**dropped** — it's body-scattering, the thing this doc's principle rejects.

---

## Leave as they are

- **`journal.cpp` (708)** — one cohesive kernel (the log + its byte codec).
  The codec primitives (`putU16`/`getU32`/…) are ~50 lines of one-liners bound
  to the wire format; a header for them buys nothing. Not a grab-bag.
- **`agent_status.cpp` (591)** — its decomposition is already real-module work:
  **C2** (`ansi.h` + `time_util.h`) and **PS2** (move `agentColor` /
  `hasPresenceFile` out). Do those, not a new item.
- **`sub_monitor.cpp` / `sub_log.cpp` rendering** — single cohesive viewers
  once M5/M3 lift the reusable readers/formatters out. Splitting a lone `render`
  scatters one concern.

---

## Recommended order

1. **M1** (`process`) — deletes the most duplication; unblocks pane + lifecycle. **[LANDED `5c462d4f`]**
2. **M4** (`pane_parse`) — unlocks unit tests on the brittle parse logic. **[LANDED `1b1c491e`]**
3. **M2** (`peer_registry`) — real domain type; de-dups broker too. **[LANDED `568a8799`]**
4. **M5** (`context_stats`) — CTX reader out of the monitor render. **[LANDED `d53b4589`]**
5. **M3** (extend C2's `time_util`) — blocked: needs C2 (`time_util.h`) to land first (lives in structural-review.md).
6. **M6** (`rpc_client`) — opportunistic, low value.

Not on this list: the **delivery.cpp / broker.cpp elephants** belong to
broker-seam-redesign.md (kernel-touching, gated, supervised), and **Transport is
a settled no-extract** — see the elephants section above.

Each landed step is a new header (a new boundary), every existing call site
became a client of it, and the build stayed green + force-verified per commit.
