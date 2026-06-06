#include "harness.h"
#include "topic_log.h"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace bus;

namespace {

auto tmpPath() -> std::string {
  static int n = 0;
  return std::string{"/tmp/claude-bus-test-topic-"} + std::to_string(++n) +
         ".log";
}

auto readBytes(const std::string& path) -> std::vector<std::byte> {
  std::ifstream in{path, std::ios::binary};
  std::vector<char> tmp((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(tmp.size());
  if (!tmp.empty()) std::memcpy(out.data(), tmp.data(), tmp.size());
  return out;
}

}  // namespace

// Append real records, read the raw file, and parse it back — the wire
// format round-trips faithfully (sender, body, protocol, offsets).
TEST(parsefrom_roundtrips_appended_records) {
  const auto path = tmpPath();
  topic::TopicLog log{path};
  topic::SendOpts opts;
  (void)log.append("alice", "first", opts);
  (void)log.append("bob", "second body", opts);
  (void)log.append("carol", "third", opts);

  const auto buf = readBytes(path);
  const auto msgs = topic::parseFrom(buf, topic::kFileHeaderBytes);
  CHECK_EQ(msgs.size(), std::size_t{3});
  CHECK_EQ(msgs[0].sender, std::string{"alice"});
  CHECK_EQ(msgs[0].body, std::string{"first"});
  CHECK_EQ(msgs[1].sender, std::string{"bob"});
  CHECK_EQ(msgs[1].body, std::string{"second body"});
  CHECK_EQ(msgs[2].body, std::string{"third"});
  // Offsets chain: each record's next_offset is the next record's offset.
  CHECK_EQ(msgs[0].next_offset, msgs[1].offset);
  CHECK_EQ(msgs[1].next_offset, msgs[2].offset);

  std::remove(path.c_str());
}

// The binary analogue of the partial-line guard: a record whose tail is
// cut off must be refused, leaving the prior whole records intact.
TEST(parsefrom_refuses_truncated_tail) {
  const auto path = tmpPath();
  topic::TopicLog log{path};
  topic::SendOpts opts;
  (void)log.append("a", "one", opts);
  (void)log.append("b", "two", opts);
  (void)log.append("c", "three", opts);

  auto buf = readBytes(path);
  CHECK(buf.size() > 1);
  // Chop one byte off the end → the last record can't be fully parsed.
  buf.resize(buf.size() - 1);
  const auto msgs = topic::parseFrom(buf, topic::kFileHeaderBytes);
  CHECK_EQ(msgs.size(), std::size_t{2});  // third refused, first two kept

  std::remove(path.c_str());
}

namespace {
// Stateful short-write hook (kernel-0-0 test): the FIRST write writes a real
// partial prefix (so the file genuinely grows — a torn record), then the next
// write fails hard (ENOSPC), forcing append()'s ftruncate-back rollback.
int g_hook_calls = 0;
auto shortThenFailWrite(int fd, const void* buf, std::size_t n) -> ssize_t {
  if (g_hook_calls++ == 0) {
    const std::size_t partial = n > 4 ? 4 : 1;  // < n → a short write
    return ::write(fd, buf, partial);
  }
  errno = ENOSPC;
  return -1;  // hard error → append rolls back the partial
}
}  // namespace

// A short ::write must leave NO torn record: append rolls the log back to its
// pre-write size, so parseFrom still yields exactly the pre-existing records
// (no misframed tail). kernel-0-0.
TEST(append_rolls_back_torn_record_on_short_write) {
  const auto path = tmpPath();
  topic::TopicLog log{path};
  topic::SendOpts opts;
  (void)log.append("alice", "keepme", opts);  // a good pre-existing record
  const auto size_before = static_cast<long>(readBytes(path).size());

  g_hook_calls = 0;
  topic::setWriteHookForTesting(shortThenFailWrite);
  auto r = log.append("bob", "this record will tear mid-write", opts);
  topic::setWriteHookForTesting(nullptr);  // restore ::write

  CHECK(!r.has_value());  // append reported the failure
  // Rolled back to exactly the pre-append size — the partial bytes are gone.
  CHECK_EQ(static_cast<long>(readBytes(path).size()), size_before);
  // The log still parses to just the pre-existing record — no torn tail.
  const auto msgs = log.dump();
  CHECK(msgs.has_value());
  CHECK_EQ(msgs->size(), std::size_t{1});
  if (msgs->size() == 1) CHECK_EQ((*msgs)[0].body, std::string{"keepme"});

  std::remove(path.c_str());
}

TEST(parsefrom_respects_start_offset) {
  const auto path = tmpPath();
  topic::TopicLog log{path};
  topic::SendOpts opts;
  (void)log.append("a", "one", opts);
  (void)log.append("b", "two", opts);

  const auto buf = readBytes(path);
  const auto all = topic::parseFrom(buf, topic::kFileHeaderBytes);
  CHECK_EQ(all.size(), std::size_t{2});
  // Resume from the second record's offset → only it comes back.
  const auto tail = topic::parseFrom(buf, all[1].offset);
  CHECK_EQ(tail.size(), std::size_t{1});
  CHECK_EQ(tail[0].body, std::string{"two"});

  std::remove(path.c_str());
}

TEST(parsefrom_respects_limit) {
  const auto path = tmpPath();
  topic::TopicLog log{path};
  topic::SendOpts opts;
  (void)log.append("a", "one", opts);
  (void)log.append("b", "two", opts);
  (void)log.append("c", "three", opts);

  const auto buf = readBytes(path);
  const auto two = topic::parseFrom(buf, topic::kFileHeaderBytes, 2);
  CHECK_EQ(two.size(), std::size_t{2});

  std::remove(path.c_str());
}

TEST(parsefrom_empty_or_headerless_buffer) {
  CHECK_EQ(topic::parseFrom({}, 0).size(), std::size_t{0});
  // A buffer smaller than the 64-byte file header yields nothing.
  std::vector<std::byte> tiny(10);
  CHECK_EQ(topic::parseFrom(tiny, 0).size(), std::size_t{0});
}

// ── trimHead (seam cut #1: the Log owns the byte rewrite) ───────────────────

// Trimming the head to a record boundary drops exactly that prefix, returns
// the byte shift, and leaves the surviving tail records intact + re-addressed
// from the header (the property the dispatch-loop cursor rebase relies on).
TEST(trimhead_drops_head_keeps_tail) {
  const auto path = tmpPath();
  topic::TopicLog log{path};
  topic::SendOpts opts;
  (void)log.append("a", "first", opts);
  (void)log.append("b", "second", opts);
  (void)log.append("c", "third", opts);

  auto before = log.dump();
  CHECK(before.has_value());
  CHECK_EQ(before->size(), std::size_t{3});
  const auto cut = (*before)[1].offset;  // drop record 0, keep 1 + 2

  const auto dropped = log.trimHead(cut);
  CHECK_EQ(dropped, cut - static_cast<std::int64_t>(topic::kFileHeaderBytes));

  auto after = log.dump();
  CHECK(after.has_value());
  CHECK_EQ(after->size(), std::size_t{2});
  CHECK_EQ((*after)[0].body, std::string{"second"});
  CHECK_EQ((*after)[1].body, std::string{"third"});
  // The surviving tail is re-addressed from the header — the first kept
  // record now begins exactly at the header.
  CHECK_EQ((*after)[0].offset,
           static_cast<std::int64_t>(topic::kFileHeaderBytes));
  CHECK_EQ((*after)[0].next_offset, (*after)[1].offset);

  std::remove(path.c_str());
}

// Out-of-range cuts are no-ops: ≤ header drops nothing, ≥ EOF refuses (never
// wipes the body). Returns 0 and leaves the file untouched.
TEST(trimhead_noop_out_of_range) {
  const auto path = tmpPath();
  topic::TopicLog log{path};
  topic::SendOpts opts;
  (void)log.append("a", "one", opts);
  (void)log.append("b", "two", opts);

  const auto header = static_cast<std::int64_t>(topic::kFileHeaderBytes);
  // At/before the header drops nothing; at/past EOF is refused (never wipes
  // the body). All three return 0.
  CHECK_EQ(log.trimHead(header), std::int64_t{0});
  CHECK_EQ(log.trimHead(0), std::int64_t{0});
  CHECK_EQ(log.trimHead(1'000'000'000), std::int64_t{0});

  auto after = log.dump();
  CHECK(after.has_value());
  CHECK_EQ(after->size(), std::size_t{2});  // untouched

  std::remove(path.c_str());
}

// ── advanceCursorMonotonic (cut #2: the at-least-once invariant) ────────────

// A cursor advances only forward: a backward or equal target is refused, so an
// out-of-order / duplicate ack can never rewind past already-delivered records.
TEST(advance_cursor_monotonic_never_rewinds) {
  const auto path = tmpPath() + ".cursor";
  std::remove(path.c_str());

  CHECK(topic::writeCursor(path, 100));
  CHECK(topic::advanceCursorMonotonic(path, 200));  // forward → writes
  CHECK_EQ(topic::readCursor(path), std::int64_t{200});
  CHECK(!topic::advanceCursorMonotonic(path, 150));  // backward → refused
  CHECK_EQ(topic::readCursor(path), std::int64_t{200});
  CHECK(!topic::advanceCursorMonotonic(path, 200));  // equal → refused (no-op)
  CHECK_EQ(topic::readCursor(path), std::int64_t{200});
  CHECK(topic::advanceCursorMonotonic(path, 250));  // forward again → writes
  CHECK_EQ(topic::readCursor(path), std::int64_t{250});

  std::remove(path.c_str());
}
