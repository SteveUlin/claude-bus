#include "harness.h"
#include "tail_reader.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace bus;

// ---- the pure splitter: the torn-tail invariant in isolation ----

TEST(split_empty_buffer) {
  auto r = splitCompleteLines("");
  CHECK_EQ(r.lines.size(), std::size_t{0});
  CHECK_EQ(r.consumed, std::size_t{0});
}

TEST(split_complete_lines) {
  auto r = splitCompleteLines("a\nbb\nccc\n");
  CHECK_EQ(r.lines.size(), std::size_t{3});
  CHECK_EQ(r.lines[0], std::string_view{"a"});
  CHECK_EQ(r.lines[1], std::string_view{"bb"});
  CHECK_EQ(r.lines[2], std::string_view{"ccc"});
  CHECK_EQ(r.consumed, std::size_t{9});  // all bytes consumed
}

// The core guard: a trailing line with no '\n' is a partial write —
// neither returned nor consumed, so the caller re-sees it whole later.
TEST(split_refuses_torn_tail) {
  auto r = splitCompleteLines("whole\npartial-no-newline");
  CHECK_EQ(r.lines.size(), std::size_t{1});
  CHECK_EQ(r.lines[0], std::string_view{"whole"});
  CHECK_EQ(r.consumed, std::size_t{6});  // only "whole\n"
}

TEST(split_preserves_empty_lines) {
  // Empty lines are kept; skipping them is the consumer's call.
  auto r = splitCompleteLines("a\n\nb\n");
  CHECK_EQ(r.lines.size(), std::size_t{3});
  CHECK_EQ(r.lines[1], std::string_view{""});
}

TEST(split_bare_partial_only) {
  auto r = splitCompleteLines("no newline at all");
  CHECK_EQ(r.lines.size(), std::size_t{0});
  CHECK_EQ(r.consumed, std::size_t{0});
}

// ---- the file-backed reader: offset stability across appends ----

namespace {

auto tmpPath() -> std::string {
  // Unique-ish path under the test's state dir; no Date/rand needed —
  // the line count keeps successive temp files distinct within a run.
  static int n = 0;
  return std::string{"/tmp/claude-bus-test-tail-"} + std::to_string(++n) +
         ".log";
}

auto writeFile(const std::string& path, std::string_view content) -> void {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

auto appendFile(const std::string& path, std::string_view content) -> void {
  std::ofstream out{path, std::ios::binary | std::ios::app};
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

TEST(tailreader_reads_then_resumes) {
  const auto path = tmpPath();
  writeFile(path, "one\ntwo\n");
  TailReader tr{path};

  auto first = tr.poll();
  CHECK_EQ(first.size(), std::size_t{2});
  CHECK_EQ(first[0], std::string{"one"});
  CHECK_EQ(first[1], std::string{"two"});

  // No new bytes → nothing, offset unchanged.
  auto empty = tr.poll();
  CHECK_EQ(empty.size(), std::size_t{0});

  // Append more → only the new lines come back.
  appendFile(path, "three\n");
  auto second = tr.poll();
  CHECK_EQ(second.size(), std::size_t{1});
  CHECK_EQ(second[0], std::string{"three"});

  std::remove(path.c_str());
}

// A torn record split across two appends must surface exactly once, whole.
TEST(tailreader_torn_record_surfaces_once) {
  const auto path = tmpPath();
  writeFile(path, "complete\npar");  // "par" is a partial line
  TailReader tr{path};

  auto first = tr.poll();
  CHECK_EQ(first.size(), std::size_t{1});
  CHECK_EQ(first[0], std::string{"complete"});  // "par" withheld

  // The writer finishes the line; the whole record arrives now.
  appendFile(path, "tial\n");
  auto second = tr.poll();
  CHECK_EQ(second.size(), std::size_t{1});
  CHECK_EQ(second[0], std::string{"partial"});

  std::remove(path.c_str());
}

TEST(tailreader_seek_to_end_ignores_history) {
  const auto path = tmpPath();
  writeFile(path, "old1\nold2\n");
  TailReader tr{path};
  tr.seekToEnd();  // mirror scanEvents' first-tick behavior

  CHECK_EQ(tr.poll().size(), std::size_t{0});  // history ignored

  appendFile(path, "new\n");
  auto fresh = tr.poll();
  CHECK_EQ(fresh.size(), std::size_t{1});
  CHECK_EQ(fresh[0], std::string{"new"});

  std::remove(path.c_str());
}

TEST(tailreader_missing_file_is_empty) {
  TailReader tr{"/tmp/claude-bus-test-does-not-exist.log"};
  CHECK_EQ(tr.poll().size(), std::size_t{0});
  CHECK_EQ(tr.offset(), std::int64_t{0});
}
