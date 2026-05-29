#pragma once

// TailReader — the offset-stable, partial-line-safe append-log tail
// read, extracted into one reusable, table-tested component.
//
// Three places in the broker reimplement this dance today
// (`delivery.cpp::scanEvents`, `delivery.cpp::maybeScanTokens`, and the
// binary analogue in `topic_log.cpp::parseFrom`). Two invariants every
// copy must hold (see docs/deep/broker-internals-cpp.md §2.1):
//
//   1. Never re-read consumed bytes — persist a byte offset, resume.
//   2. Never act on a torn record — a writer may have flushed half a
//      line; detect the partial tail and do NOT advance past it, so the
//      next poll re-sees it whole.
//
// The pure core is `splitCompleteLines`: it splits a byte chunk into
// complete (newline-terminated) lines and reports exactly how many
// bytes were consumed — the trailing partial line is neither returned
// nor counted. That function is the unit under test; the file-backed
// `TailReader` is a thin wrapper that owns the offset and reads the new
// region with `pread`-style positioned I/O.
//
// Deliberately tracks the offset as an explicit `std::int64_t` advanced
// by byte count, NOT via `std::ifstream::tellg()`. `tellg` returns -1
// after the `getline` that sets eofbit, which the inline copies dodge
// by reading the offset *before* the failing getline — correct but
// fragile and text-mode-coupled (a `\r\n` in a payload desyncs the
// count). This component sidesteps that class of bug entirely.
//
// NOTE: this is the extracted reference implementation. The broker's
// three inline copies are intentionally NOT yet rewired onto it — that
// migration (roadmap Tier 1.8) touches the live delivery path and is
// held back so this change is zero-behavior-change. New code should use
// this; the inline copies migrate under a separate, tested change.

#include <sys/stat.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

struct SplitResult {
  // Views into the input buffer, one per complete line, with the
  // trailing '\n' stripped. Empty lines are preserved (the consumer
  // decides whether to skip them).
  std::vector<std::string_view> lines;
  // Bytes through and including the last '\n'. Anything after it is a
  // partial line — left unconsumed so the caller re-sees it whole.
  std::size_t consumed{0};
};

// Pure. No I/O, no state. The torn-tail invariant in one function.
inline auto splitCompleteLines(std::string_view buf) -> SplitResult {
  SplitResult r;
  std::size_t start = 0;
  while (true) {
    const auto nl = buf.find('\n', start);
    if (nl == std::string_view::npos) break;  // remainder is a partial line
    r.lines.push_back(buf.substr(start, nl - start));
    start = nl + 1;
  }
  r.consumed = start;
  return r;
}

class TailReader {
 public:
  explicit TailReader(std::string path) : path_{std::move(path)} {}

  auto offset() const -> std::int64_t { return offset_; }
  auto setOffset(std::int64_t o) -> void { offset_ = o; }

  // Skip existing history: set the offset to the current file size so
  // the first poll() returns only lines appended after this point.
  // Mirrors scanEvents' first-tick `events_offset_ = size`.
  auto seekToEnd() -> void { offset_ = fileSize(); }

  // Return complete lines appended since the last poll, advancing the
  // offset only past whole lines. A torn trailing line stays unconsumed
  // for the next poll. If the file shrank (truncation / rotation), reset
  // to read it from the top.
  auto poll() -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto size = fileSize();
    if (size < offset_) offset_ = 0;  // file rotated/truncated under us
    if (size <= offset_) return out;

    std::ifstream in{path_, std::ios::binary};
    if (!in) return out;
    in.seekg(offset_);
    std::string chunk(static_cast<std::size_t>(size - offset_), '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    chunk.resize(static_cast<std::size_t>(in.gcount()));

    const auto sr = splitCompleteLines(chunk);
    out.reserve(sr.lines.size());
    for (const auto sv : sr.lines) out.emplace_back(sv);
    offset_ += static_cast<std::int64_t>(sr.consumed);
    return out;
  }

 private:
  auto fileSize() const -> std::int64_t {
    struct stat st;
    if (::stat(path_.c_str(), &st) != 0) return 0;
    return static_cast<std::int64_t>(st.st_size);
  }

  std::string path_;
  std::int64_t offset_{0};
};

}  // namespace bus
