#include "pane.h"

#include "pane_parse.h"
#include "process.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

namespace bus {

namespace {

// TTL for the cached list-panes snapshot. See listPanesJsonCached.
auto listPanesJsonTtlMs() -> std::int64_t {
  if (const char* e = std::getenv("CLAUDE_BUS_LISTPANES_TTL_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v >= 0) return v;
  }
  // 3 s: under a loaded zellij, list-panes itself runs ~1 s, so a 1 s TTL
  // refetches back-to-back and barely helps. 3 s keeps the fetch <=~33% of
  // the loop's time while staying well within the 5 s doorbell scan, so
  // pane-existence staleness never delays a wake by more than one scan.
  return 3000;
}

// Cached raw `zellij action list-panes --json`. paneId() runs this global
// query — ~1 s under a loaded zellij — and the broker's delivery loop calls
// paneId() once per agent per scan (pane_exists, paneState's internal
// lookup, dispatch). That O(N) of identical global forks serializes into a
// multi-second scan that wedges the single-threaded loop: it never returns
// to accept(), recv-q backs up, RPC starves (the 2026-05-31 fan-out
// broker-down). Collapse every call within a TTL window to one fork. The
// loop owns this thread — no locking, same as PaneStateCache. TTL from
// $CLAUDE_BUS_LISTPANES_TTL_MS (default 1000 ms).
auto listPanesJsonCached() -> std::string {
  static std::string cached;
  static std::chrono::steady_clock::time_point at{};
  static bool valid = false;
  const auto now = std::chrono::steady_clock::now();
  if (valid && (now - at) < std::chrono::milliseconds{listPanesJsonTtlMs()}) {
    return cached;
  }
  const auto [rc, out] = process::runCapture({"zellij", "action", "list-panes",
                                     "--json"});
  // Only a SUCCESSFUL fetch refreshes the cached value. A failed/timed-out
  // query must NOT cache empty: that makes every caller in the TTL window
  // read pane-less, flapping the doorbell, skipping token scans and tripping
  // false GONE across the whole fleet. On a warm failure keep the last good
  // value and just bump the throttle clock — otherwise the delivery loop's
  // N back-to-back paneId() calls per scan all refork, re-creating the
  // serialization wedge this cache exists to prevent.
  if (rc == 0) {
    cached = out;
    at = now;
    valid = true;
  } else if (valid) {
    at = now;
  }
  return valid ? cached : std::string{};
}

}  // namespace

auto paneId(std::string_view name) -> std::string {
  // list-panes --json (cached — see listPanesJsonCached) parsed inline to
  // avoid shelling out to jq. The output is pretty-printed (one field per
  // line, `"key": value`), so the key/value separators carry a space.
  // We slice the output into per-object windows by walking `{` / `}`
  // brace depth, then check each window for matching title +
  // is_plugin=false and extract `id`.
  const std::string out = listPanesJsonCached();
  if (out.empty()) return {};

  std::vector<long> ids;

  std::string title_needle = "\"title\": \"";
  title_needle.append(name);
  title_needle.append("\"");

  // Walk objects by tracking brace depth. When depth goes 1 → 0, the
  // chunk from the matching `{` to the closing `}` is one pane object.
  std::vector<std::size_t> opens;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const char c = out[i];
    // Skip string literals so braces inside strings don't confuse us.
    if (c == '"') {
      ++i;
      while (i < out.size()) {
        if (out[i] == '\\' && i + 1 < out.size()) {
          i += 2;
          continue;
        }
        if (out[i] == '"') break;
        ++i;
      }
      continue;
    }
    if (c == '{') {
      opens.push_back(i);
      continue;
    }
    if (c == '}' && !opens.empty()) {
      const auto start = opens.back();
      opens.pop_back();
      if (opens.empty()) {
        // Top-level object: this is one pane.
        const std::string_view obj{out.data() + start, i - start + 1};
        if (obj.find(title_needle) == std::string_view::npos) continue;
        if (obj.find("\"is_plugin\": false") == std::string_view::npos) {
          continue;
        }
        const auto id_at = obj.find("\"id\":");
        if (id_at == std::string_view::npos) continue;
        std::size_t p = id_at + 5;
        while (p < obj.size() && (obj[p] == ' ' || obj[p] == ':')) ++p;
        long id = 0;
        bool any = false;
        while (p < obj.size() && obj[p] >= '0' && obj[p] <= '9') {
          id = id * 10 + (obj[p] - '0');
          ++p;
          any = true;
        }
        if (any) ids.push_back(id);
      }
    }
  }

  if (ids.size() != 1) return {};

  std::string result = "terminal_";
  result += std::to_string(ids[0]);
  return result;
}

auto sendToPane(std::string_view pane_id, std::string_view text) -> bool {
  const std::string pane_s{pane_id};
  std::string text_s{text};
  // Flatten newlines to spaces before writing (comms draft-clobber fix).
  // Claude's prompt text field never holds a legit '\n' — a soft newline is a
  // Shift+Enter at the key layer, not a buffer character — so a raw '\n' sent
  // via write-chars reads as Enter and SUBMITS a multiline message at its first
  // newline, fragmenting it and letting a concurrent human typist's keystrokes
  // merge into the fragments (the comms-only symptom: comms is the lone live
  // TTY-push agent). Treating '\n' as a space lands the whole message as ONE
  // line + the single trailing Enter (per sulin: newline is never legit input
  // here). Single-line text is unaffected.
  for (auto& c : text_s) {
    if (c == '\n') c = ' ';
  }
  // write-chars NAMED-PANE TEXT
  if (process::runSilent({"zellij", "action", "write-chars", "--pane-id",
                 pane_s.c_str(), text_s.c_str()}) != 0) {
    return false;
  }
  // send-keys NAMED-PANE "Enter"
  if (process::runSilent({"zellij", "action", "send-keys", "--pane-id",
                 pane_s.c_str(), "Enter"}) != 0) {
    return false;
  }
  return true;
}

auto sendKey(std::string_view pane_id, std::string_view key) -> bool {
  const std::string pane_s{pane_id};
  const std::string key_s{key};
  return process::runSilent({"zellij", "action", "send-keys", "--pane-id",
                    pane_s.c_str(), key_s.c_str()}) == 0;
}

auto sendToPaneSafe(std::string_view agent_name,
                    std::string_view text) -> bool {
  const std::string pane = paneId(agent_name);
  if (pane.empty()) return false;

  const auto ps = paneState(agent_name);

  // dump-screen failed entirely (pane gone or zellij hiccup). Best-
  // effort: fall through to the raw path rather than refuse — at worst
  // we recreate the prior behaviour, never worse.
  if (!ps.ok) return sendToPane(pane, text);

  // mode = unknown means the claude TUI status footer isn't visible in
  // the bottom 6 rows. That happens when:
  //   - the user scrolled up to read history
  //   - a tool produced enough output to push the footer off-screen
  //   - the pane is in some transient layout (resize handle, etc.)
  // In every case, our state read is unreliable. Deliver later instead
  // of risking a write into the wrong context. The broker's retry loop
  // will pick it up.
  if (!ps.modeKnown()) return false;

  // LOCKED: the TUI is in a modal that doesn't accept input.
  if (ps.isLocked()) return false;

  // bypass-perms is off — claude will prompt for human approval on any
  // tool use the message triggers. The chat write itself wouldn't be
  // blocked, but interrupting a human-supervised attach session with
  // a bus delivery is a bad default. Defer; broker retries. When the
  // human re-enables bypass (or the agent emits Stop, clearing any
  // permission prompt), delivery resumes. modeKnown() guarantees
  // we positively observed the footer — never a false-positive on a
  // scrolled pane.
  if (ps.bypassKnown() && !ps.bypassOn()) return false;

  // Snapshot the human-typed draft. Suggestions are claude's ghost
  // autocomplete — generated on demand from the buffer — so we don't
  // preserve them; they regenerate when the draft is retyped.
  const std::string saved = ps.hasBuffer() ? ps.buffer : std::string{};

  // Always send `i` then `Ctrl u` before writing. Rationale:
  //   - If the pane is actually in NORMAL or VISUAL (whether the
  //     marker was visible or hidden — see detectMode's fallback),
  //     `i` switches to INSERT.
  //   - If already in INSERT, `i` types a literal 'i' that pollutes
  //     the buffer — but the next Ctrl-U clears the line, including
  //     that 'i' and any prior draft, so the net effect is the same
  //     as "ensure clean INSERT-mode buffer." That's the precondition
  //     sendToPane expects.
  //   - Costs ~100 ms of paired keypress + sleep per write. The
  //     previous conditional path was clever (skip 'i' when mode ==
  //     INSERT, skip Ctrl-U on empty buffer) but it broke against
  //     claude-TUI variants that hide the explicit mode marker.
  //     Robust > clever for a write that's already gating on flock.
  sendKey(pane, "i");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  sendKey(pane, "Ctrl u");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  if (!sendToPane(pane, text)) return false;

  // Restore the user's draft via write-chars (no Enter — re-types
  // without submitting). 100 ms gap so claude has time to process the
  // Enter from sendToPane and reset the prompt before we re-key.
  if (!saved.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    process::runSilent({"zellij", "action", "write-chars", "--pane-id",
               pane.c_str(), saved.c_str()});
  }
  return true;
}

auto paneState(std::string_view name) -> PaneState {
  PaneState ps;
  const std::string pid = paneId(name);
  if (pid.empty()) return ps;

  const auto [rc, dump] = process::runCapture({"zellij", "action", "dump-screen",
                                      "--pane-id", pid.c_str(), "--ansi"});
  if (rc != 0) return ps;

  const auto plain = pane_parse::stripAnsi(dump);
  const auto lines = pane_parse::splitLines(plain);

  ps.mode = pane_parse::detectMode(lines);
  ps.bypass_perms = pane_parse::detectBypass(lines);
  // The "bypass permissions on" string lives on the same input row as
  // the `-- INSERT --` mode marker. If the mode scan came back unknown
  // (typically because the user scrolled up and the input row is off-
  // screen), the bypass scan is equally unreliable — don't claim "off"
  // when we just can't see the footer. Downstream surfaces treat empty
  // as "no opinion".
  if (ps.mode == "unknown") ps.bypass_perms = "";
  const int input_idx = pane_parse::findInputLine(lines);

  ps.buffer = "(empty)";
  ps.suggestion = "(none)";
  if (input_idx >= 0) {
    const auto ansi_line = pane_parse::extractAnsiLine(dump, input_idx);
    const auto parts = pane_parse::parseInput(ansi_line);
    if (!parts.buffer.empty()) ps.buffer = parts.buffer;
    if (!parts.suggestion.empty()) ps.suggestion = parts.suggestion;
  }

  ps.ok = !ps.mode.empty();
  return ps;
}

auto paneStateCacheTtlMs() -> std::int64_t {
  if (const char* e = std::getenv("CLAUDE_BUS_PANESTATE_TTL_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v >= 0) return v;
  }
  return 300;
}

auto paneStateCached(std::string_view name) -> PaneState {
  // One process-static cache on the broker's loop thread. The broker is
  // single-threaded, so no locking is needed; the state RPC handler runs
  // on the same thread but deliberately calls raw paneState() instead.
  static PaneStateCache cache{
      [](std::string_view n) { return paneState(n); },
      [] { return std::chrono::steady_clock::now(); },
      std::chrono::milliseconds{paneStateCacheTtlMs()}};
  return cache.get(name);
}

}  // namespace bus
