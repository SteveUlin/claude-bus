#include "pane.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace bus {

namespace {

constexpr std::string_view kPromptMarker = "\xe2\x9d\xaf";  // U+276F ❯
constexpr std::string_view kDivider = "\xe2\x94\x80";       // U+2500 ─

// Default subprocess timeout. Every zellij invocation lives on the
// broker's main pselect thread, so a hang here halts the delivery
// loop: scanEvents stops draining UPS acks, scanRetries stops firing,
// and no other RPC tick fires until the child returns. 5 s is way
// past zellij's normal latency (dump-screen returns in <100 ms) while
// still giving slow systems some headroom.
constexpr auto kDefaultSubprocessTimeout = std::chrono::milliseconds{5000};

// Wait for `pid` up to `timeout`, polling waitpid(WNOHANG) every
// ~20 ms. Returns {reaped, status}. When `reaped` is false the caller
// is responsible for SIGKILL'ing the runaway child and harvesting the
// final status — see waitWithTimeoutOrKill.
auto waitWithTimeoutOrKill(pid_t pid, std::chrono::milliseconds timeout)
    -> std::pair<bool /*timed_out*/, int /*status*/> {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) return {false, status};
    if (r < 0) {
      if (errno == EINTR) continue;
      return {false, 0};  // already reaped or other error
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  // Timed out — kill the child and reap synchronously. SIGKILL since
  // a hung `zellij action ...` is wedged on the zellij server IPC and
  // won't notice SIGTERM in a useful timeframe.
  ::kill(pid, SIGKILL);
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return {true, status};
}

// Drain a pipe FD into a string with a deadline. The fd must be
// O_NONBLOCK. Stops on EOF, deadline-exceeded, or read error.
auto slurpFdWithDeadline(int fd, std::chrono::steady_clock::time_point deadline)
    -> std::string {
  std::string out;
  std::array<char, 4096> buf{};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto n = ::read(fd, buf.data(), buf.size());
    if (n > 0) {
      out.append(buf.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) break;  // EOF
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // No data right now; sleep briefly so we don't burn CPU.
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      continue;
    }
    break;  // genuine read error
  }
  return out;
}

// Fork + exec a command with stdout captured, stderr discarded.
// Returns {exit_code, stdout}. exit_code is -1 on spawn failure OR
// when the child blew past the timeout (we SIGKILL it and continue —
// the caller decides what -1 means for its semantics).
auto runCapture(const std::vector<const char*>& argv,
                std::chrono::milliseconds timeout =
                    kDefaultSubprocessTimeout)
    -> std::pair<int, std::string> {
  int pipefd[2]{};
  if (::pipe(pipefd) != 0) return {-1, {}};

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return {-1, {}};
  }
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::close(pipefd[1]);
    // stderr to /dev/null — pane-id-ambiguous and dump-screen-failed
    // surface through exit codes, not noisy stderr leakage.
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    std::vector<char*> a;
    a.reserve(argv.size() + 1);
    for (const auto* s : argv) a.push_back(const_cast<char*>(s));
    a.push_back(nullptr);
    ::execvp(a[0], a.data());
    _exit(127);
  }

  ::close(pipefd[1]);
  // Non-blocking reads so the deadline drain doesn't get stuck on
  // a hung child that hasn't closed the pipe yet.
  const int flags = ::fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto out = slurpFdWithDeadline(pipefd[0], deadline);
  ::close(pipefd[0]);

  // Remaining time after the drain. If we already hit the deadline
  // drain-side, waitWithTimeoutOrKill gets a zero budget and kills
  // immediately — exactly what we want.
  const auto wait_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  const auto [timed_out, status] = waitWithTimeoutOrKill(
      pid, wait_budget.count() > 0 ? wait_budget
                                   : std::chrono::milliseconds{0});
  if (timed_out) return {-1, std::move(out)};
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, std::move(out)};
}

// Run a command with no stdin/stdout/stderr captured (silent). Returns
// the exit code (-1 on spawn failure or timeout). Used by sendToPane.
auto runSilent(const std::vector<const char*>& argv,
               std::chrono::milliseconds timeout =
                   kDefaultSubprocessTimeout) -> int {
  const pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    std::vector<char*> a;
    a.reserve(argv.size() + 1);
    for (const auto* s : argv) a.push_back(const_cast<char*>(s));
    a.push_back(nullptr);
    ::execvp(a[0], a.data());
    _exit(127);
  }
  const auto [timed_out, status] = waitWithTimeoutOrKill(pid, timeout);
  if (timed_out) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Trim leading and trailing whitespace, including UTF-8 NBSP (Claude
// Code uses U+00A0 to pad the input area; without this rule an "empty"
// input reads as a stray non-empty byte sequence).
auto trim(std::string s) -> std::string {
  auto is_ws = [&](std::size_t i) -> std::size_t {
    if (i >= s.size()) return 0;
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    if (c == 0xc2 && i + 1 < s.size() &&
        static_cast<unsigned char>(s[i + 1]) == 0xa0) {
      return 2;  // UTF-8 NBSP
    }
    return 0;
  };
  std::size_t b = 0;
  while (auto n = is_ws(b)) b += n;
  std::size_t e = b;
  std::size_t last = b;
  while (e < s.size()) {
    if (auto n = is_ws(e)) {
      e += n;
    } else {
      ++e;
      last = e;
    }
  }
  return s.substr(b, last - b);
}

// Cut at the first box-drawing horizontal (U+2500). The wrapping row
// can land the trailing divider on the same physical line as the `❯`
// prompt; those bytes are decoration, not buffer content.
auto cutAtDivider(std::string s) -> std::string {
  const auto pos = s.find(kDivider);
  if (pos != std::string::npos) s.resize(pos);
  return s;
}

// Walk an ANSI-laced byte stream, calling `emit(text, dim)` for each
// run of printable bytes between escape sequences. `dim` is set when
// the most recent SGR for that run includes the dim attribute (`2`) or
// a foreground color whose luminance is in the dark-grey range Claude
// Code uses for ghost suggestions.
template <typename Emit>
auto walkAnsi(std::string_view s, Emit emit) -> void {
  bool dim_attr = false;
  bool dim_color = false;
  std::string run;

  auto flush = [&] {
    if (run.empty()) return;
    emit(std::string_view{run}, dim_attr || dim_color);
    run.clear();
  };

  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      flush();
      std::size_t j = i + 2;
      while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e)) ++j;
      if (j >= s.size()) break;
      const char final_byte = s[j];
      const std::string_view params{s.data() + i + 2, j - (i + 2)};
      i = j + 1;
      if (final_byte != 'm') continue;

      std::vector<int> nums;
      {
        int cur = 0;
        bool have = false;
        for (char c : params) {
          if (c == ';' || c == ':') {
            nums.push_back(have ? cur : 0);
            cur = 0;
            have = false;
          } else if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            have = true;
          }
        }
        nums.push_back(have ? cur : 0);
      }

      for (std::size_t k = 0; k < nums.size(); ++k) {
        const int n = nums[k];
        if (n == 0) {
          dim_attr = false;
          dim_color = false;
        } else if (n == 2) {
          dim_attr = true;
        } else if (n == 22) {
          dim_attr = false;
        } else if (n == 39) {
          dim_color = false;
        } else if (n == 38 && k + 1 < nums.size()) {
          const int mode = nums[k + 1];
          if (mode == 5 && k + 2 < nums.size()) {
            const int idx = nums[k + 2];
            // xterm 256 palette: 232..255 is a 24-step greyscale ramp.
            // Treat the bottom third as "ghost text".
            dim_color = idx >= 232 && idx <= 243;
            k += 2;
          } else if (mode == 2 && k + 4 < nums.size()) {
            const int r = nums[k + 2];
            const int g = nums[k + 3];
            const int b = nums[k + 4];
            // Dark-grey heuristic: all channels low and roughly equal.
            const int hi = std::max({r, g, b});
            const int lo = std::min({r, g, b});
            dim_color = hi <= 120 && (hi - lo) <= 20;
            k += 4;
          }
        }
      }
      continue;
    }
    if (s[i] == '\n') {
      flush();
      emit(std::string_view{"\n"}, false);
      ++i;
      continue;
    }
    run.push_back(s[i++]);
  }
  flush();
}

auto stripAnsi(std::string_view s) -> std::string {
  std::string out;
  out.reserve(s.size());
  walkAnsi(s, [&](std::string_view chunk, bool) { out.append(chunk); });
  return out;
}

auto splitLines(std::string_view s) -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') {
      lines.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < s.size()) lines.emplace_back(s.substr(start));
  return lines;
}

auto isDividerLine(std::string_view line) -> bool {
  std::size_t hits = 0;
  for (std::size_t i = 0; i + kDivider.size() <= line.size(); ++i) {
    if (line.substr(i, kDivider.size()) == kDivider) ++hits;
  }
  return hits >= 10;
}

// Find the bottom input area: the last line containing `❯` AND sitting
// between divider lines. Returns line index or -1 if not found.
auto findInputLine(const std::vector<std::string>& lines) -> int {
  for (int i = static_cast<int>(lines.size()) - 1; i >= 1; --i) {
    if (lines[i].find(kPromptMarker) == std::string::npos) continue;
    if (i > 0 && isDividerLine(lines[i - 1])) return i;
  }
  return -1;
}

auto detectMode(const std::vector<std::string>& lines) -> std::string {
  // Scan the whole screen for `-- WORD --` with one of the known modes.
  // Claude often renders the mode marker on row N while padding rows
  // N+1..end with blanks, so a fixed bottom-N window misses it and
  // returns "unknown" — which then falsely trips BootStuck downstream.
  // The needle is specific to the claude footer.
  constexpr std::array<std::string_view, 4> kModes{"INSERT", "NORMAL", "VISUAL",
                                                   "LOCKED"};
  for (const auto& l : lines) {
    for (auto mode : kModes) {
      std::string needle = "-- ";
      needle.append(mode);
      needle.append(" --");
      if (l.find(needle) != std::string::npos) return std::string{mode};
    }
  }
  // Fallback: claude TUI sometimes hides the explicit `-- INSERT --`
  // marker in the bottom status row (e.g., when the row displays the
  // `← for agents` hint instead). In that case we still observe the
  // canonical "bypass permissions" footer string, which means the
  // status footer IS visible — we just can't see the mode marker.
  // Treat that as INSERT: the dominant claude-TUI state, and the
  // downstream sendToPaneSafe still issues `i` + Ctrl-U before any
  // write so a false-positive (rare NORMAL/VISUAL with marker hidden)
  // self-corrects. Without this fallback, every delivery to a pane
  // showing the alt-status variant defers forever — that was the
  // bast-wedge symptom 2026-05-28.
  for (const auto& l : lines) {
    if (l.find("bypass permissions") != std::string::npos) return "INSERT";
  }
  return "unknown";
}

auto detectBypass(const std::vector<std::string>& lines) -> std::string {
  // The string is unique to claude's status footer. A tall pane can push
  // the footer out of any fixed bottom-N window (long tool output, an
  // active permission prompt overlay, trailing blank rows), so scan the
  // whole visible screen — dump-screen is bounded to pane height, no
  // scrollback, so this stays cheap.
  for (const auto& l : lines) {
    if (l.find("bypass permissions on") != std::string::npos) return "on";
  }
  return "off";
}

auto extractAnsiLine(std::string_view dump, int target_line) -> std::string {
  int line = 0;
  std::size_t start = 0;
  std::size_t i = 0;
  while (i < dump.size() && line < target_line) {
    if (dump[i] == '\n') {
      ++line;
      start = i + 1;
    }
    ++i;
  }
  std::size_t end = start;
  while (end < dump.size() && dump[end] != '\n') ++end;
  return std::string{dump.substr(start, end - start)};
}

struct InputParts {
  std::string buffer;
  std::string suggestion;
};

auto parseInput(std::string_view ansi_line) -> InputParts {
  InputParts parts;
  bool past_prompt = false;
  std::string pending;
  walkAnsi(ansi_line, [&](std::string_view chunk, bool dim) {
    if (chunk == "\n") return;
    if (!past_prompt) {
      pending.append(chunk);
      const auto pos = pending.find(kPromptMarker);
      if (pos == std::string::npos) return;
      past_prompt = true;
      std::size_t rest = pos + kPromptMarker.size();
      while (rest < pending.size() && pending[rest] == ' ') ++rest;
      if (rest < pending.size()) parts.buffer.append(pending.substr(rest));
      return;
    }
    if (dim) {
      parts.suggestion.append(chunk);
    } else {
      parts.buffer.append(chunk);
    }
  });
  parts.buffer = trim(cutAtDivider(std::move(parts.buffer)));
  parts.suggestion = trim(cutAtDivider(std::move(parts.suggestion)));

  // Cursor-on-suggestion fix: when Claude Code shows a ghost
  // suggestion, the cursor sits ON the first character, inverting
  // colors. That one char renders bright while the rest stays dim.
  // Our classifier then splits "add per-record CRC-32C" into
  // buffer="a" + suggestion="dd per-record CRC-32C". Re-merge when
  // buffer is exactly one character AND a suggestion follows.
  if (parts.buffer.size() == 1 && !parts.suggestion.empty()) {
    parts.suggestion = parts.buffer + parts.suggestion;
    parts.buffer.clear();
  }
  return parts;
}

}  // namespace

auto paneId(std::string_view name) -> std::string {
  // zellij action list-panes --json — we parse it ourselves to avoid
  // shelling out to jq. The output is pretty-printed (one field per
  // line, `"key": value`), so the key/value separators carry a space.
  // We slice the output into per-object windows by walking `{` / `}`
  // brace depth, then check each window for matching title +
  // is_plugin=false and extract `id`.
  const auto [rc, out] = runCapture({"zellij", "action", "list-panes",
                                     "--json"});
  if (rc != 0) return {};

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
  const std::string text_s{text};
  // write-chars NAMED-PANE TEXT
  if (runSilent({"zellij", "action", "write-chars", "--pane-id",
                 pane_s.c_str(), text_s.c_str()}) != 0) {
    return false;
  }
  // send-keys NAMED-PANE "Enter"
  if (runSilent({"zellij", "action", "send-keys", "--pane-id",
                 pane_s.c_str(), "Enter"}) != 0) {
    return false;
  }
  return true;
}

auto sendKey(std::string_view pane_id, std::string_view key) -> bool {
  const std::string pane_s{pane_id};
  const std::string key_s{key};
  return runSilent({"zellij", "action", "send-keys", "--pane-id",
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
  if (ps.mode == "unknown") return false;

  // LOCKED: the TUI is in a modal that doesn't accept input.
  if (ps.mode == "LOCKED") return false;

  // bypass-perms is off — claude will prompt for human approval on any
  // tool use the message triggers. The chat write itself wouldn't be
  // blocked, but interrupting a human-supervised attach session with
  // a bus delivery is a bad default. Defer; broker retries. When the
  // human re-enables bypass (or the agent emits Stop, clearing any
  // permission prompt), delivery resumes. mode != "unknown" guarantees
  // we positively observed the footer — never a false-positive on a
  // scrolled pane.
  if (ps.bypass_perms == "off") return false;

  // Snapshot the human-typed draft. Suggestions are claude's ghost
  // autocomplete — generated on demand from the buffer — so we don't
  // preserve them; they regenerate when the draft is retyped.
  const std::string saved =
      (ps.buffer != "(empty)") ? ps.buffer : std::string{};

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
    runSilent({"zellij", "action", "write-chars", "--pane-id",
               pane.c_str(), saved.c_str()});
  }
  return true;
}

auto paneState(std::string_view name) -> PaneState {
  PaneState ps;
  const std::string pid = paneId(name);
  if (pid.empty()) return ps;

  const auto [rc, dump] = runCapture({"zellij", "action", "dump-screen",
                                      "--pane-id", pid.c_str(), "--ansi"});
  if (rc != 0) return ps;

  const auto plain = stripAnsi(dump);
  const auto lines = splitLines(plain);

  ps.mode = detectMode(lines);
  ps.bypass_perms = detectBypass(lines);
  // The "bypass permissions on" string lives on the same input row as
  // the `-- INSERT --` mode marker. If the mode scan came back unknown
  // (typically because the user scrolled up and the input row is off-
  // screen), the bypass scan is equally unreliable — don't claim "off"
  // when we just can't see the footer. Downstream surfaces treat empty
  // as "no opinion".
  if (ps.mode == "unknown") ps.bypass_perms = "";
  const int input_idx = findInputLine(lines);

  ps.buffer = "(empty)";
  ps.suggestion = "(none)";
  if (input_idx >= 0) {
    const auto ansi_line = extractAnsiLine(dump, input_idx);
    const auto parts = parseInput(ansi_line);
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
