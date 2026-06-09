#include "harness.h"
#include "cursor_store.h"
#include "journal.h"
#include "state_paths.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace bus;

namespace {

auto makeStateRoot(const char* prefix) -> std::string {
  static int n = 0;
  const std::string sr = std::string{prefix} + std::to_string(++n);
  std::filesystem::create_directories(sr + "/topics");
  return sr;
}

}  // namespace

// ── CursorStore: at-least-once never-rewind invariant ─────────────────────────

// CursorStore::ack enforces forward-only advancement: a backward or equal
// target is a no-op (returns false) and leaves the stored cursor unchanged.
TEST(cursor_store_ack_never_rewinds) {
  const std::string state_root =
      makeStateRoot("/tmp/claude-bus-test-cs-mono-");

  auto log_r = Journal::openOrCreate(topicLogPath(state_root, "ack-mono"));
  CHECK(log_r.has_value());
  auto& log = *log_r;
  CursorStore cursors{state_root, "ack-mono"};

  // Append three records so we have real cursors to work with.
  (void)log.append(bytesOf("r1"));
  (void)log.append(bytesOf("r2"));
  (void)log.append(bytesOf("r3"));

  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK_EQ(recs->size(), std::size_t{3});

  const auto ca0 = Journal::cursorAfter((*recs)[0]);  // after r1
  const auto ca1 = Journal::cursorAfter((*recs)[1]);  // after r2
  const auto ca2 = Journal::cursorAfter((*recs)[2]);  // after r3

  // Start: consumer cursor is start-of-log (default).
  CHECK_EQ(cursors.consumerCursor(""), Journal::Cursor{});

  // Advance to ca0 — should succeed (forward move).
  CHECK(cursors.ack("", ca0));
  CHECK_EQ(cursors.consumerCursor(""), ca0);

  // Advance to ca1 — forward, should succeed.
  CHECK(cursors.ack("", ca1));
  CHECK_EQ(cursors.consumerCursor(""), ca1);

  // Try to rewind to ca0 — backward, must be refused (returns false, cursor
  // unchanged).
  CHECK(!cursors.ack("", ca0));
  CHECK_EQ(cursors.consumerCursor(""), ca1);

  // Equal target — no-op, returns false, cursor unchanged.
  CHECK(!cursors.ack("", ca1));
  CHECK_EQ(cursors.consumerCursor(""), ca1);

  // Forward again to ca2 — should succeed.
  CHECK(cursors.ack("", ca2));
  CHECK_EQ(cursors.consumerCursor(""), ca2);

  std::filesystem::remove_all(state_root);
}

// ── CursorStore: same-journal ack succeeds; unbound cursor accepted ───────────

// The negative (cross-journal fatal) path calls abort() and cannot be tested
// here without fork. This test covers:
//   - same-journal ack succeeds and advances the cursor.
//   - an unbound (tag-0) cursor is accepted by any journal.
TEST(cursor_store_cross_journal_ack_refused) {
  const std::string sr_a =
      makeStateRoot("/tmp/claude-bus-test-cs-xj-a-");
  const std::string sr_b =
      makeStateRoot("/tmp/claude-bus-test-cs-xj-b-");

  auto a_r = Journal::openOrCreate(topicLogPath(sr_a, "journal-a"));
  auto b_r = Journal::openOrCreate(topicLogPath(sr_b, "journal-b"));
  CHECK(a_r.has_value());
  CHECK(b_r.has_value());
  auto& a = *a_r;
  auto& b = *b_r;
  CursorStore cs_a{sr_a, "journal-a"};
  CursorStore cs_b{sr_b, "journal-b"};

  (void)a.append(bytesOf("a1"));
  (void)a.append(bytesOf("a2"));
  (void)b.append(bytesOf("b1"));
  (void)b.append(bytesOf("b2"));

  auto a_recs = a.dump();
  CHECK(a_recs.has_value());
  CHECK_EQ(a_recs->size(), std::size_t{2});

  const auto cursor_from_a = Journal::cursorAfter((*a_recs)[0]);

  // cs_a.ack with cursor_from_a must succeed (same journal, matching tag).
  CHECK(cs_a.ack("", cursor_from_a));
  CHECK_EQ(cs_a.consumerCursor(""), cursor_from_a);

  // An unbound (tag-0) cursor is accepted by any named journal — it represents
  // a legacy / unknown origin and is treated as valid. Construct via the
  // "<offset>:0" canonical form.
  const auto tok = Journal::cursorToToken((*a_recs)[0].position);
  const auto offset_only = tok.substr(0, tok.find(':'));  // decimal offset
  const auto unbound = Journal::cursorFromToken(offset_only + ":0");  // tag 0
  CHECK(cs_b.ack("", unbound));  // unbound is accepted by any journal

  std::filesystem::remove_all(sr_a);
  std::filesystem::remove_all(sr_b);
}

// ── CursorStore: consumerCursor carries the journal's tag ─────────────────────

// consumerCursor on a journal that has been written must return a Cursor with
// the journal's non-zero UUID-derived tag. Before the first write the tag is 0
// (the file doesn't exist yet).
TEST(cursor_store_consumer_cursor_tag) {
  const std::string state_root =
      makeStateRoot("/tmp/claude-bus-test-cs-tag-");

  auto log_r = Journal::openOrCreate(topicLogPath(state_root, "tag-topic"));
  CHECK(log_r.has_value());
  auto& log = *log_r;
  CursorStore cursors{state_root, "tag-topic"};

  // Before any write: file absent → tag is 0, cursor is start-of-log.
  const auto pre = cursors.consumerCursor();
  CHECK_EQ(pre, Journal::Cursor{});

  // After first append: file has a UUID → tag must be non-zero.
  (void)log.append(bytesOf("msg"));
  const auto expected_tag = Journal::tagOf(topicLogPath(state_root, "tag-topic"));
  CHECK(expected_tag != 0);

  // consumerCursor (still at start-of-log, no ack) must carry the tag.
  const auto after = cursors.consumerCursor();
  const auto tok = Journal::cursorToToken(after);
  CHECK(tok.find(':') != std::string::npos);
  const auto tag_part = tok.substr(tok.find(':') + 1);
  CHECK_EQ(std::stoull(tag_part), expected_tag);

  std::filesystem::remove_all(state_root);
}

// ── CursorStore: lastAckedId round-trip ───────────────────────────────────────

// ack with a non-empty id stamps lastAckedId; subsequent reads return it.
TEST(cursor_store_last_acked_id) {
  const std::string state_root =
      makeStateRoot("/tmp/claude-bus-test-cs-lastid-");

  auto log_r = Journal::openOrCreate(topicLogPath(state_root, "lastid-topic"));
  CHECK(log_r.has_value());
  auto& log = *log_r;
  CursorStore cursors{state_root, "lastid-topic"};

  (void)log.append(bytesOf("a"));
  (void)log.append(bytesOf("b"));

  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK_EQ(recs->size(), std::size_t{2});

  const auto ca0 = Journal::cursorAfter((*recs)[0]);
  const auto id0 = (*recs)[0].id;

  // Empty before any ack.
  CHECK_EQ(cursors.lastAckedId(""), std::string{});

  // Ack with the id — must be stamped.
  CHECK(cursors.ack("", ca0, id0));
  CHECK_EQ(cursors.lastAckedId(""), id0);

  std::filesystem::remove_all(state_root);
}

// ── (a) write-then-read round-trip: offset+tag persist to the cursor file ────
//
// After ack(), the on-disk .cursor file must use the new "<offset>:<tag>\n"
// text format. A subsequent consumerCursor() must return a cursor that carries
// the same offset AND tag.
TEST(cursor_store_tagged_roundtrip) {
  const std::string state_root =
      makeStateRoot("/tmp/claude-bus-test-cs-rt-");

  auto log_r = Journal::openOrCreate(topicLogPath(state_root, "rt-topic"));
  CHECK(log_r.has_value());
  auto& log = *log_r;
  CursorStore cursors{state_root, "rt-topic"};

  (void)log.append(bytesOf("x"));
  (void)log.append(bytesOf("y"));

  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK_EQ(recs->size(), std::size_t{2});

  const auto ca0 = Journal::cursorAfter((*recs)[0]);
  CHECK(cursors.ack("rt", ca0));

  // Read the raw cursor file — must contain "<offset>:<tag>\n".
  const std::string cursor_path =
      state_root + "/cursors/rt-topic/rt.cursor";
  std::ifstream f{cursor_path};
  CHECK(f.is_open());
  std::string line;
  std::getline(f, line);
  const auto colon = line.find(':');
  CHECK(colon != std::string::npos);
  const auto tag_str = line.substr(colon + 1);
  const auto file_tag = static_cast<std::uint64_t>(std::stoull(tag_str));
  CHECK(file_tag != 0);

  // The tag in the file must match the journal's own tag.
  const auto journal_tag = Journal::tagOf(topicLogPath(state_root, "rt-topic"));
  CHECK_EQ(file_tag, journal_tag);

  // consumerCursor must return the same cursor (same offset and same tag).
  const auto read_back = cursors.consumerCursor("rt");
  const auto tok_ack = Journal::cursorToToken(ca0);
  const auto tok_read = Journal::cursorToToken(read_back);
  CHECK_EQ(tok_ack, tok_read);

  std::filesystem::remove_all(state_root);
}

// ── (b) legacy raw-offset file is adopted, not reset ─────────────────────────
//
// A pre-existing cursor file in the old 8-byte little-endian u64 format must
// be read as a valid offset (tag 0 = unbound). The offset is adopted as-is
// and is NOT reset to start-of-log.
TEST(cursor_store_legacy_file_adopted) {
  const std::string state_root =
      makeStateRoot("/tmp/claude-bus-test-cs-leg-");

  // Create the journal so tagOf() returns a non-zero tag.
  auto log_r = Journal::openOrCreate(topicLogPath(state_root, "legacy-topic"));
  CHECK(log_r.has_value());
  auto& log = *log_r;
  (void)log.append(bytesOf("rec"));

  // Determine a valid non-zero byte offset inside the log.
  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK_EQ(recs->size(), std::size_t{1});
  const auto ca0 = Journal::cursorAfter((*recs)[0]);
  const auto tok = Journal::cursorToToken(ca0);
  // Extract the raw byte offset from the token (decimal before ':').
  const auto colon = tok.find(':');
  CHECK(colon != std::string::npos);
  const std::uint64_t raw_offset =
      static_cast<std::uint64_t>(std::stoull(tok.substr(0, colon)));
  CHECK(raw_offset > 0);

  // Write the cursor file in the old 8-byte LE u64 format.
  const std::string cursor_dir =
      state_root + "/cursors/legacy-topic";
  std::filesystem::create_directories(cursor_dir);
  const std::string cursor_path = cursor_dir + "/_default.cursor";
  {
    const int fd =
        ::open(cursor_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    std::uint8_t buf[8];
    for (int i = 0; i < 8; ++i)
      buf[i] = static_cast<std::uint8_t>((raw_offset >> (i * 8)) & 0xFF);
    (void)::write(fd, buf, 8);
    ::close(fd);
  }

  // consumerCursor must return the stored offset — NOT start-of-log.
  CursorStore cursors{state_root, "legacy-topic"};
  const auto c = cursors.consumerCursor();
  const auto read_tok = Journal::cursorToToken(c);
  const auto read_colon = read_tok.find(':');
  CHECK(read_colon != std::string::npos);
  const auto read_offset =
      static_cast<std::uint64_t>(std::stoull(read_tok.substr(0, read_colon)));
  CHECK_EQ(read_offset, raw_offset);

  std::filesystem::remove_all(state_root);
}

// ── (c) stale cursor (tag mismatch) resets to start-of-log ───────────────────
//
// A cursor written against journal A must be discarded when the journal file
// is replaced by journal B (new UUID → new tag). consumerCursor must return
// start-of-log (offset 0) rather than applying the stale offset from journal A.
TEST(cursor_store_stale_tag_resets_to_start) {
  const std::string state_root =
      makeStateRoot("/tmp/claude-bus-test-cs-stale-");

  const std::string topic_path = topicLogPath(state_root, "stale-topic");

  // ── Phase 1: build journal A and advance the cursor into it. ──────────────
  {
    auto log_r = Journal::openOrCreate(topic_path);
    CHECK(log_r.has_value());
    auto& log = *log_r;
    (void)log.append(bytesOf("a1"));
    (void)log.append(bytesOf("a2"));
    auto recs = log.dump();
    CHECK(recs.has_value());
    CursorStore cs{state_root, "stale-topic"};
    const auto ca1 = Journal::cursorAfter((*recs)[1]);
    CHECK(cs.ack("", ca1));
    // Sanity: cursor is past the header.
    const auto c = cs.consumerCursor();
    const auto tok = Journal::cursorToToken(c);
    const auto colon = tok.find(':');
    CHECK(colon != std::string::npos);
    const auto off =
        static_cast<std::uint64_t>(std::stoull(tok.substr(0, colon)));
    CHECK(off > 0);
  }

  // ── Phase 2: delete journal A and create journal B at the same path. ──────
  std::filesystem::remove(topic_path);
  {
    auto log_r = Journal::create(topic_path);
    CHECK(log_r.has_value());
    auto& log = *log_r;
    (void)log.append(bytesOf("b1"));
  }

  // The new journal must have a different tag.
  const auto tag_b = Journal::tagOf(topic_path);
  CHECK(tag_b != 0);

  // ── Phase 3: stale cursor must be discarded — reset to start-of-log. ──────
  CursorStore cs2{state_root, "stale-topic"};
  const auto c2 = cs2.consumerCursor();
  // Offset must be 0 (start-of-log), not the non-zero offset from journal A.
  const auto tok2 = Journal::cursorToToken(c2);
  const auto colon2 = tok2.find(':');
  CHECK(colon2 != std::string::npos);
  const auto off2 =
      static_cast<std::uint64_t>(std::stoull(tok2.substr(0, colon2)));
  CHECK_EQ(off2, std::uint64_t{0});

  std::filesystem::remove_all(state_root);
}
