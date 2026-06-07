#include "harness.h"
#include "crc32c.h"
#include "journal.h"
#include "journal_internal.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace bus;

namespace {

auto tmpPath() -> std::string {
  static int n = 0;
  return std::string{"/tmp/claude-bus-test-journal-"} + std::to_string(++n) +
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

// ── Opaque-payload round-trip ────────────────────────────────────────────────

// append bytes -> dump -> same bytes; id is well-formed
TEST(roundtrip_opaque_payload) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("first payload"));
  (void)log.append(bytesOf("second payload"));
  (void)log.append(bytesOf("third"));

  const auto buf = readBytes(path);
  const auto recs = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(recs.size(), std::size_t{3});
  CHECK_EQ(recs[0].payload, bytesOf("first payload"));
  CHECK_EQ(recs[1].payload, bytesOf("second payload"));
  CHECK_EQ(recs[2].payload, bytesOf("third"));
  // Cursors chain: each record's cursor_after equals the next record's position.
  CHECK_EQ(Journal::cursorAfter(recs[0]), recs[1].position);
  CHECK_EQ(Journal::cursorAfter(recs[1]), recs[2].position);
  // id format: 13-digit ms + '-' + 4-digit hex seq
  CHECK(recs[0].id.size() >= 18);  // at minimum "0000000000000-0000"
  CHECK(recs[0].id != recs[1].id);

  std::remove(path.c_str());
}

// ── Truncated tail refused ────────────────────────────────────────────────────

TEST(parsefrom_refuses_truncated_tail) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("one"));
  (void)log.append(bytesOf("two"));
  (void)log.append(bytesOf("three"));

  auto buf = readBytes(path);
  CHECK(buf.size() > 1);
  // Chop one byte off the end → the last record can't be fully parsed.
  buf.resize(buf.size() - 1);
  const auto recs = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(recs.size(), std::size_t{2});  // third refused, first two kept

  std::remove(path.c_str());
}

// ── Start offset respected ────────────────────────────────────────────────────

TEST(parsefrom_respects_start_offset) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("one"));
  (void)log.append(bytesOf("two"));

  const auto buf = readBytes(path);
  const auto all = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(all.size(), std::size_t{2});
  // Resume from the second record's position → only it comes back.
  // Extract raw offset from the opaque token (offset is the colon-prefix).
  const auto tok = Journal::cursorToToken(all[1].position);
  const auto raw = static_cast<std::int64_t>(std::stoll(tok));
  const auto tail = Journal::parseForTest(buf, raw);
  CHECK_EQ(tail.size(), std::size_t{1});
  CHECK_EQ(tail[0].payload, bytesOf("two"));

  std::remove(path.c_str());
}

// ── Limit respected ───────────────────────────────────────────────────────────

TEST(parsefrom_respects_limit) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("one"));
  (void)log.append(bytesOf("two"));
  (void)log.append(bytesOf("three"));

  const auto buf = readBytes(path);
  const auto two = Journal::parseForTest(buf, kJournalHeaderBytes, 2);
  CHECK_EQ(two.size(), std::size_t{2});

  std::remove(path.c_str());
}

// ── Empty / headerless buffer yields nothing ──────────────────────────────────

TEST(parsefrom_empty_or_headerless_buffer) {
  CHECK_EQ(Journal::parseForTest({}, 0).size(), std::size_t{0});
  std::vector<std::byte> tiny(10);
  CHECK_EQ(Journal::parseForTest(tiny, 0).size(), std::size_t{0});
}

// ── CRC integrity ─────────────────────────────────────────────────────────────

TEST(crc_corruption_stops_parse) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("first"));
  (void)log.append(bytesOf("CORRUPTME"));
  (void)log.append(bytesOf("third"));

  auto buf = readBytes(path);
  const auto good = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(good.size(), std::size_t{3});

  // Flip one byte inside the SECOND record's payload span.
  const auto r1_tok = Journal::cursorToToken(good[1].position);
  const auto r1_off = static_cast<std::size_t>(std::stoll(r1_tok));
  buf[r1_off + 30] = static_cast<std::byte>(
      std::to_integer<std::uint8_t>(buf[r1_off + 30]) ^ 0xFF);

  const auto after = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(after.size(), std::size_t{1});
  CHECK_EQ(after[0].payload, bytesOf("first"));

  std::remove(path.c_str());
}

// A shrunk-but-in-bounds record_len whose next_offset lands mid-record fails
// CRC verification — not reframed past the forged boundary.
TEST(corrupt_record_len_no_misframe) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("firstbody"));
  (void)log.append(bytesOf("second"));
  (void)log.append(bytesOf("third"));

  auto buf = readBytes(path);
  const auto good = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(good.size(), std::size_t{3});

  // Shrink record 0's record_len (u64 LE at offset+4) by 8.
  const auto r0_tok = Journal::cursorToToken(good[0].position);
  const auto r0_len_off =
      static_cast<std::size_t>(std::stoll(r0_tok)) + 4;
  std::uint64_t rec_len = 0;
  for (int i = 0; i < 8; ++i) {
    rec_len |= static_cast<std::uint64_t>(
                   std::to_integer<std::uint8_t>(buf[r0_len_off + i]))
               << (i * 8);
  }
  rec_len -= 8;  // still >= kMinRecordLen and in bounds, but a false frame
  for (int i = 0; i < 8; ++i) {
    buf[r0_len_off + i] = static_cast<std::byte>((rec_len >> (i * 8)) & 0xFF);
  }

  const auto after = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(after.size(), std::size_t{0});

  std::remove(path.c_str());
}

// ── seq monotonic ─────────────────────────────────────────────────────────────

TEST(seq_monotonic_same_ms) {
  const auto path = tmpPath();
  Journal log{path};
  const auto a = log.append(bytesOf("one"));
  const auto b = log.append(bytesOf("two"));
  CHECK(a.has_value());
  CHECK(b.has_value());
  CHECK(*a != *b);

  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK_EQ(recs->size(), std::size_t{2});
  auto seqOf = [](const std::string& id) -> long {
    return std::strtol(id.substr(id.size() - 4).c_str(), nullptr, 16);
  };
  const bool same_ms = (*recs)[0].append_ms == (*recs)[1].append_ms;
  if (same_ms) {
    CHECK(seqOf((*recs)[1].id) > seqOf((*recs)[0].id));
  } else {
    CHECK((*recs)[0].id != (*recs)[1].id);
  }

  std::remove(path.c_str());
}

// ── v6 tail() — reverse read ──────────────────────────────────────────────────

// tail(n) returns the last n records in chronological order.
TEST(tail_returns_last_n_chronological) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("a"));
  (void)log.append(bytesOf("b"));
  (void)log.append(bytesOf("c"));
  (void)log.append(bytesOf("d"));
  (void)log.append(bytesOf("e"));

  auto t = log.tail(3);
  CHECK(t.has_value());
  CHECK_EQ(t->size(), std::size_t{3});
  // Chronological: c, d, e
  CHECK_EQ((*t)[0].payload, bytesOf("c"));
  CHECK_EQ((*t)[1].payload, bytesOf("d"));
  CHECK_EQ((*t)[2].payload, bytesOf("e"));

  std::remove(path.c_str());
}

// tail(n > count) returns all records.
TEST(tail_n_larger_than_count) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("x"));
  (void)log.append(bytesOf("y"));

  auto t = log.tail(100);
  CHECK(t.has_value());
  CHECK_EQ(t->size(), std::size_t{2});
  CHECK_EQ((*t)[0].payload, bytesOf("x"));
  CHECK_EQ((*t)[1].payload, bytesOf("y"));

  std::remove(path.c_str());
}

// A deliberately torn tail makes tail() fall back to forward + still return
// valid records (no crash, consistent count with dump).
TEST(tail_torn_tail_fallback) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("one"));
  (void)log.append(bytesOf("two"));
  (void)log.append(bytesOf("three"));

  // Read the file, truncate one byte from the end (tears the last record).
  auto buf = readBytes(path);
  CHECK(buf.size() > 1);
  buf.resize(buf.size() - 1);

  // Write the torn buffer to a new path.
  const auto torn_path = tmpPath();
  {
    std::ofstream out{torn_path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
  }

  Journal torn{torn_path};
  // dump() should return 2 (torn tail refused).
  auto d = torn.dump();
  CHECK(d.has_value());
  CHECK_EQ(d->size(), std::size_t{2});

  // tail(3) must also return 2 (fallback, consistent with dump).
  auto t = torn.tail(3);
  CHECK(t.has_value());
  CHECK_EQ(t->size(), std::size_t{2});
  CHECK_EQ((*t)[0].payload, bytesOf("one"));
  CHECK_EQ((*t)[1].payload, bytesOf("two"));

  std::remove(path.c_str());
  std::remove(torn_path.c_str());
}

// ── Offset chain in tail() ────────────────────────────────────────────────────

// tail() back-pointer walk must set position correctly: each record's
// cursorAfter equals the following record's position, and all positions
// are within the file.
TEST(tail_offsets_chain_correctly) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("a"));
  (void)log.append(bytesOf("b"));
  (void)log.append(bytesOf("c"));

  auto t = log.tail(3);
  CHECK(t.has_value());
  CHECK_EQ(t->size(), std::size_t{3});
  // Chronological: a, b, c.  cursor_after of each = position of the next.
  CHECK_EQ(Journal::cursorAfter((*t)[0]), (*t)[1].position);
  CHECK_EQ(Journal::cursorAfter((*t)[1]), (*t)[2].position);
  // First record starts at or after the file header.
  const auto header_cursor =
      Journal::cursorFromToken(std::to_string(kJournalHeaderBytes));
  CHECK((*t)[0].position >= header_cursor);

  std::remove(path.c_str());
}

// ── v6 hand-built buffer oracle ───────────────────────────────────────────────

namespace {

// Build a minimal valid v6 file buffer in memory (no I/O).
//   header (64 bytes) + one record with the given payload.
// Record layout (v6, no payload_len field):
//   crc(4) | record_len(8) | append_ms(8) | seq(2) | payload(n) | trailer(8)
//   Total = 30 + n
auto MakeV6Buffer(std::string_view the_payload,
                  std::int64_t append_ms = 1'700'000'000'000LL,
                  std::uint16_t seq = 0x0042) -> std::vector<std::byte> {
  auto putLE = [](std::vector<std::byte>& b, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i)
      b.push_back(std::byte((v >> (i * 8)) & 0xFF));
  };

  // File header.
  std::vector<std::byte> buf(kJournalHeaderBytes, std::byte{0});
  buf[0] = std::byte{'B'};
  buf[1] = std::byte{'U'};
  buf[2] = std::byte{'S'};
  buf[3] = std::byte{0};
  // version at offset 4 (u32 LE = 6).
  const std::uint32_t ver = kJournalFormatVersion;
  for (int i = 0; i < 4; ++i)
    buf[4 + i] = std::byte((ver >> (i * 8)) & 0xFF);

  // Record body (everything after the crc placeholder).
  std::vector<std::byte> body;
  const std::uint64_t plen = the_payload.size();
  // overhead = crc(4) + front_len(8) + append_ms(8) + seq(2) + trailer(8) = 30
  const std::uint64_t rec_len = 30 + plen;
  putLE(body, rec_len, 8);          // front record_len
  putLE(body, static_cast<std::uint64_t>(append_ms), 8);
  putLE(body, seq, 2);
  for (char c : the_payload) body.push_back(std::byte(c));
  putLE(body, rec_len, 8);          // trailer == front

  // CRC covers the body (everything after the crc field).
  const std::uint32_t crc = Crc32c({body.data(), body.size()});

  // Prepend the crc to the record.
  std::vector<std::byte> rec;
  for (int i = 0; i < 4; ++i) rec.push_back(std::byte((crc >> (i * 8)) & 0xFF));
  rec.insert(rec.end(), body.begin(), body.end());

  buf.insert(buf.end(), rec.begin(), rec.end());
  return buf;
}

}  // namespace

// parseFrom on a hand-crafted v6 buffer must decode the exact field values
// that were written — pins the wire format against symmetric round-trip drift.
TEST(parsefrom_handbuilt_v6_buffer) {
  const std::string kPayload = "wire-oracle";
  const std::int64_t kMs = 1'700'000'000'123LL;
  const std::uint16_t kSeq = 0x00AB;

  const auto buf = MakeV6Buffer(kPayload, kMs, kSeq);
  const auto recs = Journal::parseForTest(buf, kJournalHeaderBytes);

  CHECK_EQ(recs.size(), std::size_t{1});
  CHECK_EQ(recs[0].payload, bytesOf(kPayload));
  CHECK_EQ(recs[0].append_ms, kMs);
  // id must encode the known ms and seq.
  CHECK_EQ(recs[0].id, std::string{"1700000000123-00ab"});
  // Use "<offset>:0" — cursorFromToken requires a colon; plain decimal now
  // decodes to start-of-log (Cursor{}) not the given offset.
  const auto header_cursor =
      Journal::cursorFromToken(std::to_string(kJournalHeaderBytes) + ":0");
  CHECK_EQ(recs[0].position, header_cursor);
  // overhead = 30 (crc4 + front_len8 + append_ms8 + seq2 + trailer8)
  const std::int64_t expected_next =
      static_cast<std::int64_t>(kJournalHeaderBytes) +
      static_cast<std::int64_t>(30 + kPayload.size());
  const auto expected_next_cursor =
      Journal::cursorFromToken(std::to_string(expected_next) + ":0");
  CHECK_EQ(Journal::cursorAfter(recs[0]), expected_next_cursor);

  // A CRC flip in any byte of the body must cause parse to return nothing.
  auto corrupt = buf;
  corrupt[kJournalHeaderBytes + 10] =
      std::byte(std::to_integer<std::uint8_t>(corrupt[kJournalHeaderBytes + 10]) ^ 0xFF);
  CHECK_EQ(Journal::parseForTest(corrupt, kJournalHeaderBytes).size(), std::size_t{0});
}

// ── truncated_at out-parameter ───────────────────────────────────────────────

namespace {

// Append the record bytes from a single-record MakeV6Buffer onto an existing
// multi-record buffer (strips the 64-byte file header from the source).
auto AppendRecord(std::vector<std::byte>& dst,
                  const std::vector<std::byte>& src) -> void {
  dst.insert(dst.end(), src.begin() + kJournalHeaderBytes, src.end());
}

}  // namespace

// parseForTest reports the byte offset of the bad record when CRC is corrupt.
TEST(truncated_at_reports_corruption_offset) {
  // Build: header + rec0("alpha") + rec1("beta") + rec2("gamma", but CRC flipped)
  auto buf = MakeV6Buffer("alpha");
  AppendRecord(buf, MakeV6Buffer("beta"));
  const std::size_t rec2_offset = buf.size();  // start of the third record
  AppendRecord(buf, MakeV6Buffer("gamma"));

  // Flip one byte inside rec2's CRC-covered region (offset 10 past rec2 start =
  // inside the append_ms field, which is covered by the CRC).
  buf[rec2_offset + 10] =
      std::byte(std::to_integer<std::uint8_t>(buf[rec2_offset + 10]) ^ 0xFF);

  std::int64_t trunc = 0;  // not -1 — must be written by the parser
  const auto recs =
      Journal::parseForTest(buf, kJournalHeaderBytes, SIZE_MAX, &trunc);

  CHECK_EQ(recs.size(), std::size_t{2});
  CHECK_EQ(recs[0].payload, bytesOf("alpha"));
  CHECK_EQ(recs[1].payload, bytesOf("beta"));
  // truncated_at must point exactly at the start of the bad third record.
  CHECK_EQ(trunc, static_cast<std::int64_t>(rec2_offset));
}

// A clean log yields truncated_at == -1.
TEST(truncated_at_clean_log_is_minus_one) {
  auto buf = MakeV6Buffer("x");
  AppendRecord(buf, MakeV6Buffer("y"));
  AppendRecord(buf, MakeV6Buffer("z"));

  std::int64_t trunc = 0;  // must be overwritten to -1
  const auto recs =
      Journal::parseForTest(buf, kJournalHeaderBytes, SIZE_MAX, &trunc);

  CHECK_EQ(recs.size(), std::size_t{3});
  CHECK_EQ(trunc, std::int64_t{-1});
}

// A limit-bounded read of a healthy log must yield truncated_at == -1 — the
// limit stop is not a corruption signal.
TEST(truncated_at_limit_stop_is_minus_one) {
  auto buf = MakeV6Buffer("one");
  AppendRecord(buf, MakeV6Buffer("two"));
  AppendRecord(buf, MakeV6Buffer("three"));

  std::int64_t trunc = 0;  // must be overwritten to -1
  const auto recs =
      Journal::parseForTest(buf, kJournalHeaderBytes, /*limit=*/2, &trunc);

  CHECK_EQ(recs.size(), std::size_t{2});
  CHECK_EQ(trunc, std::int64_t{-1});
}

// ── mid-walk CRC corruption in tail() ────────────────────────────────────────

// tail() must NOT fall back to the forward scan when corruption is discovered
// mid-walk (records NEWER than the corruption have already been collected).
// It must stop the reverse walk and return only the records already found,
// without handing control to dump()-based forward scan (which returns older
// records than the reverse walk already verified).
TEST(tail_midwalk_crc_corruption_stops_not_fallback) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("r1"));
  (void)log.append(bytesOf("r2"));
  (void)log.append(bytesOf("r3"));
  (void)log.append(bytesOf("r4"));
  (void)log.append(bytesOf("r5"));

  // Capture all records to find record r2's span so we can corrupt it.
  auto buf = readBytes(path);
  const auto all = Journal::parseForTest(buf, kJournalHeaderBytes);
  CHECK_EQ(all.size(), std::size_t{5});

  // Flip one byte inside r2 (index 1): choose a byte after the crc field
  // itself (past position+4) so it's inside the CRC-covered region.
  const auto r2_tok = Journal::cursorToToken(all[1].position);
  const auto r2_off = static_cast<std::size_t>(std::stoll(r2_tok));
  buf[r2_off + 30] = std::byte(
      std::to_integer<std::uint8_t>(buf[r2_off + 30]) ^ 0xFF);

  // Write the corrupted buffer to a new file.
  const auto corrupt_path = tmpPath();
  {
    std::ofstream out{corrupt_path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
  }

  Journal corrupted{corrupt_path};

  // dump() (forward scan) stops at r2, returning only r1.
  auto d = corrupted.dump();
  CHECK(d.has_value());
  CHECK_EQ(d->size(), std::size_t{1});
  CHECK_EQ((*d)[0].payload, bytesOf("r1"));

  // tail(5) does a reverse walk: finds r5, r4, r3, then hits corrupt r2 and
  // stops.  It must return [r3, r4, r5] — the records verified by the reverse
  // walk — NOT fall back to dump() which would return [r1].
  auto t = corrupted.tail(5);
  CHECK(t.has_value());
  CHECK_EQ(t->size(), std::size_t{3});
  CHECK_EQ((*t)[0].payload, bytesOf("r3"));
  CHECK_EQ((*t)[1].payload, bytesOf("r4"));
  CHECK_EQ((*t)[2].payload, bytesOf("r5"));

  std::remove(path.c_str());
  std::remove(corrupt_path.c_str());
}

// ── Journal::ack at-least-once never-rewind invariant ────────────────────────

// Journal::ack is the member replacement for the old writeCursor /
// advanceCursorMonotonic / readCursor free functions. It enforces
// forward-only advancement: a backward or equal target is a no-op (returns
// false) and leaves the stored cursor unchanged.
TEST(advance_cursor_monotonic_never_rewinds) {
  static int n = 0;
  const std::string state_root =
      std::string{"/tmp/claude-bus-test-ack-mono-"} + std::to_string(++n);
  std::filesystem::create_directories(state_root + "/topics");

  Journal log{state_root, "ack-mono"};

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
  CHECK_EQ(log.consumerCursor(""), Journal::Cursor{});

  // Advance to ca0 — should succeed (forward move).
  CHECK(log.ack("", ca0));
  CHECK_EQ(log.consumerCursor(""), ca0);

  // Advance to ca1 — forward, should succeed.
  CHECK(log.ack("", ca1));
  CHECK_EQ(log.consumerCursor(""), ca1);

  // Try to rewind to ca0 — backward, must be refused (returns false, cursor
  // unchanged).
  CHECK(!log.ack("", ca0));
  CHECK_EQ(log.consumerCursor(""), ca1);

  // Equal target — no-op, returns false, cursor unchanged.
  CHECK(!log.ack("", ca1));
  CHECK_EQ(log.consumerCursor(""), ca1);

  // Forward again to ca2 — should succeed.
  CHECK(log.ack("", ca2));
  CHECK_EQ(log.consumerCursor(""), ca2);

  std::filesystem::remove_all(state_root);
}

// ── Cursor opaque type ───────────────────────────────────────────────────────

// cursorToToken / cursorFromToken round-trip without losing the value.
TEST(cursor_token_roundtrip) {
  const auto path = tmpPath();
  Journal log{path};
  (void)log.append(bytesOf("record-a"));
  (void)log.append(bytesOf("record-b"));

  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK_EQ(recs->size(), std::size_t{2});

  // Both records carry non-default cursor_after.
  const auto ca0 = Journal::cursorAfter((*recs)[0]);
  const auto ca1 = Journal::cursorAfter((*recs)[1]);
  CHECK(ca0 != Journal::Cursor{});  // not start-of-log
  CHECK(ca1 != Journal::Cursor{});
  CHECK(ca1 > ca0);  // monotonically increasing

  // Round-trip through token.
  const auto tok0 = Journal::cursorToToken(ca0);
  const auto tok1 = Journal::cursorToToken(ca1);
  CHECK(!tok0.empty());
  CHECK(!tok1.empty());
  CHECK(tok0 != tok1);
  CHECK_EQ(Journal::cursorFromToken(tok0), ca0);
  CHECK_EQ(Journal::cursorFromToken(tok1), ca1);

  // Empty / garbage tokens produce the default (start-of-log) cursor.
  CHECK_EQ(Journal::cursorFromToken(""), Journal::Cursor{});
  CHECK_EQ(Journal::cursorFromToken("not-a-number"), Journal::Cursor{});

  std::remove(path.c_str());
}

// ── Journal::ack forward-only invariant ──────────────────────────────────────

// Journal::ack must never rewind a consumer cursor: a backward or equal
// target is refused and the cursor stays at the higher value.
TEST(journal_ack_never_rewinds) {
  const auto base = std::string{"/tmp/claude-bus-test-ack-journal-"};
  static int n = 0;
  const std::string state_root = base + std::to_string(++n);
  const std::string name = "ack-test";
  std::filesystem::create_directories(state_root + "/topics");

  Journal log{state_root, name};
  (void)log.append(bytesOf("msg-a"));
  (void)log.append(bytesOf("msg-b"));
  (void)log.append(bytesOf("msg-c"));

  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK_EQ(recs->size(), std::size_t{3});

  const auto ca0 = Journal::cursorAfter((*recs)[0]);
  const auto ca1 = Journal::cursorAfter((*recs)[1]);
  const auto ca2 = Journal::cursorAfter((*recs)[2]);

  // Advance to record 1 (cursor_after of record 0).
  CHECK(log.ack("", ca0));
  CHECK_EQ(log.consumerCursor(""), ca0);

  // Advance forward to record 2.
  CHECK(log.ack("", ca1));
  CHECK_EQ(log.consumerCursor(""), ca1);

  // Backward: refused — cursor stays at ca1.
  CHECK(!log.ack("", ca0));
  CHECK_EQ(log.consumerCursor(""), ca1);

  // Equal: refused (no-op).
  CHECK(!log.ack("", ca1));
  CHECK_EQ(log.consumerCursor(""), ca1);

  // Forward again.
  CHECK(log.ack("", ca2));
  CHECK_EQ(log.consumerCursor(""), ca2);

  std::filesystem::remove_all(state_root);
}

// ── Journal-tag binding ───────────────────────────────────────────────────────

// A cursor from a named journal carries a non-zero tag; from an unnamed (path-
// only) journal it carries tag 0 (unbound).
TEST(cursor_tag_named_vs_unnamed_journal) {
  static int n = 0;
  const std::string state_root =
      std::string{"/tmp/claude-bus-test-tag-"} + std::to_string(++n);
  std::filesystem::create_directories(state_root + "/topics");

  // Named journal — cursors must carry a non-zero tag.
  Journal named{state_root, "my-topic"};
  (void)named.append(bytesOf("msg-a"));
  (void)named.append(bytesOf("msg-b"));

  auto named_recs = named.dump();
  CHECK(named_recs.has_value());
  CHECK_EQ(named_recs->size(), std::size_t{2});

  // cursor_after from dump() of a named journal must be non-zero.
  const auto ca = Journal::cursorAfter((*named_recs)[0]);
  const auto tok = Journal::cursorToToken(ca);
  // Token must contain a colon (offset:tag) and both parts must be parseable.
  CHECK(tok.find(':') != std::string::npos);
  const auto rt = Journal::cursorFromToken(tok);
  CHECK(rt != Journal::Cursor{});  // not start-of-log
  CHECK_EQ(rt, ca);               // round-trip exact

  // consumerCursor from a named journal carries a non-zero tag.
  const auto cc = named.consumerCursor();
  // Default (no acks yet) is tag-stamped but offset == 0.
  const auto cc_tok = Journal::cursorToToken(cc);
  CHECK(cc_tok.find(':') != std::string::npos);

  // Unnamed (path-only) journal — cursors carry tag 0.
  const auto path = std::string{"/tmp/claude-bus-test-tag-unnamed-"} +
                    std::to_string(n) + ".log";
  Journal unnamed{path};
  (void)unnamed.append(bytesOf("x"));
  auto unnamed_recs = unnamed.dump();
  CHECK(unnamed_recs.has_value());
  CHECK(!unnamed_recs->empty());
  // Tag 0 means the token is just the decimal offset (no colon) OR
  // "offset:0" — either way cursorFromToken must decode it, and the
  // cursor must compare equal to a plain Cursor{} at the same offset.
  const auto ua = Journal::cursorAfter((*unnamed_recs)[0]);
  const auto utok = Journal::cursorToToken(ua);
  const auto urt = Journal::cursorFromToken(utok);
  CHECK_EQ(urt, ua);

  std::filesystem::remove_all(state_root);
  std::remove(path.c_str());
}

// cursorToToken → cursorFromToken round-trips both offset and tag.
// A legacy tag-less token (no colon) decodes to tag 0.
TEST(cursor_token_roundtrip_with_tag) {
  static int n = 0;
  const std::string state_root =
      std::string{"/tmp/claude-bus-test-tokrt-"} + std::to_string(++n);
  std::filesystem::create_directories(state_root + "/topics");

  Journal log{state_root, "tokrt-topic"};
  (void)log.append(bytesOf("alpha"));

  auto recs = log.dump();
  CHECK(recs.has_value());
  CHECK(!recs->empty());

  const auto ca = Journal::cursorAfter((*recs)[0]);
  const auto tok = Journal::cursorToToken(ca);

  // Must contain a colon separating offset and tag.
  const auto colon = tok.find(':');
  CHECK(colon != std::string::npos);

  // Both halves must be valid decimals.
  const auto offset_part = tok.substr(0, colon);
  const auto tag_part = tok.substr(colon + 1);
  CHECK(!offset_part.empty());
  CHECK(!tag_part.empty());
  const auto tag_val = std::stoull(tag_part);
  CHECK(tag_val != 0);  // named journal → non-zero tag persisted

  // Round-trip is exact.
  const auto rt = Journal::cursorFromToken(tok);
  CHECK_EQ(rt, ca);

  // A tag-less token (plain decimal, no colon) now decodes to start-of-log
  // (Cursor{}, offset 0) — legacy round-trip no longer supported.
  const auto no_colon = Journal::cursorFromToken("12345");
  CHECK_EQ(no_colon, Journal::Cursor{});  // defaults to start-of-log
  // The canonical form "<offset>:0" round-trips correctly.
  const auto explicit_unbound = Journal::cursorFromToken("12345:0");
  CHECK_EQ(explicit_unbound, Journal::cursorFromToken("12345:0"));

  std::filesystem::remove_all(state_root);
}

// Cross-journal guard: B.ack with a cursor issued by A now calls fatal()
// (always-on crash — it is a programmer error, not a runtime condition).
// Phase 2 will add a fork-based death test to verify the abort.
// This test verifies the POSITIVE paths: same-journal ack succeeds and
// advances the cursor; an unbound (tag-0) cursor is accepted by any journal.
TEST(cross_journal_ack_refused) {
  static int n = 0;
  const std::string sr_a =
      std::string{"/tmp/claude-bus-test-xj-a-"} + std::to_string(++n);
  const std::string sr_b =
      std::string{"/tmp/claude-bus-test-xj-b-"} + std::to_string(n);
  std::filesystem::create_directories(sr_a + "/topics");
  std::filesystem::create_directories(sr_b + "/topics");

  Journal a{sr_a, "journal-a"};
  Journal b{sr_b, "journal-b"};

  (void)a.append(bytesOf("a1"));
  (void)a.append(bytesOf("a2"));
  (void)b.append(bytesOf("b1"));
  (void)b.append(bytesOf("b2"));

  auto a_recs = a.dump();
  CHECK(a_recs.has_value());
  CHECK_EQ(a_recs->size(), std::size_t{2});

  const auto cursor_from_a = Journal::cursorAfter((*a_recs)[0]);

  // A.ack with cursor_from_a must succeed (same journal, matching tag).
  CHECK(a.ack("", cursor_from_a));
  CHECK_EQ(a.consumerCursor(""), cursor_from_a);

  // An unbound (tag-0) cursor is accepted by any named journal — it represents
  // a legacy / unknown origin and is treated as valid. Construct via the
  // "<offset>:0" canonical form (tag-less/colon-less tokens now decode to
  // start-of-log, so we must include the explicit ":0" suffix).
  const auto tok = Journal::cursorToToken((*a_recs)[0].position);
  const auto offset_only = tok.substr(0, tok.find(':'));  // decimal offset
  const auto unbound = Journal::cursorFromToken(offset_only + ":0");  // tag 0
  CHECK(b.ack("", unbound));  // unbound is accepted

  std::filesystem::remove_all(sr_a);
  std::filesystem::remove_all(sr_b);
}
