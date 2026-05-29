#include "harness.h"
#include "topic_log.h"

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
