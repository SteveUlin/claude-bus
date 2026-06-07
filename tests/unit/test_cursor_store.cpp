#include "harness.h"
#include "cursor_store.h"
#include "journal.h"
#include "state_paths.h"

#include <cstdlib>
#include <filesystem>
#include <string>

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

  Journal log{topicLogPath(state_root, "ack-mono")};
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

  Journal a{topicLogPath(sr_a, "journal-a")};
  Journal b{topicLogPath(sr_b, "journal-b")};
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

  Journal log{topicLogPath(state_root, "tag-topic")};
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

  Journal log{topicLogPath(state_root, "lastid-topic")};
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
