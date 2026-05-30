#include "harness.h"
#include "retention.h"

#include <cstdint>
#include <vector>

using namespace bus;
using bus::retention::planTrim;
using bus::retention::rebaseCursor;
using bus::retention::RecordMeta;

namespace {

// Four 100-byte records starting after a 64-byte header. Offsets:
//   r0 [64,164)  r1 [164,264)  r2 [264,364)  r3 [364,464)
// sent_ms ascending: 1000, 2000, 3000, 4000.
auto fourRecords() -> std::vector<RecordMeta> {
  return {
      {64, 164, 1000},
      {164, 264, 2000},
      {264, 364, 3000},
      {364, 464, 4000},
  };
}

constexpr std::int64_t kHeader = 64;

}  // namespace

// ---- planTrim: nothing to do ----

TEST(retention_empty_log_no_trim) {
  auto p = planTrim({}, /*now*/ 9999, /*ret*/ 100, /*max*/ 100, kHeader,
                    /*min_cursor*/ 0, /*clamp*/ false);
  CHECK_EQ(p.cut_offset, kHeader);
  CHECK_EQ(p.dropped_bytes, std::int64_t{0});
}

TEST(retention_disabled_rules_no_trim) {
  // retention_ms=0 and max_bytes=0 → both rules off → keep everything.
  auto p = planTrim(fourRecords(), /*now*/ 1'000'000, 0, 0, kHeader, 0, false);
  CHECK_EQ(p.cut_offset, kHeader);
  CHECK_EQ(p.dropped_bytes, std::int64_t{0});
}

// ---- planTrim: age rule (retention_ms) ----

TEST(retention_age_drops_expired_head) {
  // now=3500, retention=1000 → expiry boundary = sent_ms < 2500.
  // r0(1000) r1(2000) expired; r2(3000) r3(4000) live. Cut at r2.offset=264.
  auto p = planTrim(fourRecords(), 3500, 1000, 0, kHeader, 0, false);
  CHECK_EQ(p.cut_offset, std::int64_t{264});
  CHECK_EQ(p.dropped_bytes, std::int64_t{200});
}

TEST(retention_age_all_expired_keeps_none_past_header) {
  // Everything older than the window → cut to the last record's
  // next_offset (drop all four).
  auto p = planTrim(fourRecords(), 100'000, 1000, 0, kHeader, 0, false);
  CHECK_EQ(p.cut_offset, std::int64_t{464});
  CHECK_EQ(p.dropped_bytes, std::int64_t{400});
}

TEST(retention_age_none_expired) {
  auto p = planTrim(fourRecords(), 4000, 10'000, 0, kHeader, 0, false);
  CHECK_EQ(p.cut_offset, kHeader);
}

// ---- planTrim: size rule (max_bytes) ----

TEST(retention_size_keeps_tail_within_cap) {
  // max_bytes=200 → keep the smallest tail with span<=200. EOF=464.
  // r3: 464-364=100 ok; r2: 464-264=200 ok; r1: 464-164=300 too big.
  // Earliest record satisfying <=200 is r2 (offset 264). Cut at 264.
  auto p = planTrim(fourRecords(), 0, 0, 200, kHeader, 0, false);
  CHECK_EQ(p.cut_offset, std::int64_t{264});
  CHECK_EQ(p.dropped_bytes, std::int64_t{200});
}

TEST(retention_size_cap_above_total_no_trim) {
  auto p = planTrim(fourRecords(), 0, 0, 10'000, kHeader, 0, false);
  CHECK_EQ(p.cut_offset, kHeader);
}

TEST(retention_age_and_size_take_the_deeper_cut) {
  // age (now=3500,ret=1000) would cut at 264; size (cap=100) keeps only
  // r3 → cut at 364. The deeper cut (364) wins.
  auto p = planTrim(fourRecords(), 3500, 1000, 100, kHeader, 0, false);
  CHECK_EQ(p.cut_offset, std::int64_t{364});
}

// ---- planTrim: delivery-guarantee clamp ----

TEST(retention_clamp_protects_unconsumed) {
  // All four expired, but the consumer has only read through r1
  // (min_cursor=264). With clamp on, never drop r2/r3 even though expired.
  auto p = planTrim(fourRecords(), 100'000, 1000, 0, kHeader,
                    /*min_cursor*/ 264, /*clamp*/ true);
  CHECK_EQ(p.cut_offset, std::int64_t{264});
  CHECK_EQ(p.dropped_bytes, std::int64_t{200});
}

TEST(retention_clamp_no_cursor_no_trim) {
  // Guaranteed kind, nothing consumed (min_cursor=0). Undelivered mail is
  // never dropped, even fully expired.
  auto p = planTrim(fourRecords(), 100'000, 1000, 0, kHeader,
                    /*min_cursor*/ 0, /*clamp*/ true);
  CHECK_EQ(p.cut_offset, kHeader);
  CHECK_EQ(p.dropped_bytes, std::int64_t{0});
}

TEST(retention_no_clamp_drops_regardless_of_cursor) {
  // Fire-and-forget kind: min_cursor ignored, expiry drops everything.
  auto p = planTrim(fourRecords(), 100'000, 1000, 0, kHeader,
                    /*min_cursor*/ 64, /*clamp*/ false);
  CHECK_EQ(p.cut_offset, std::int64_t{464});
}

TEST(retention_clamp_allows_cut_up_to_cursor) {
  // Consumer read everything (min_cursor=464); expiry wants to drop all.
  // Clamp permits the full cut since all records are consumed.
  auto p = planTrim(fourRecords(), 100'000, 1000, 0, kHeader,
                    /*min_cursor*/ 464, /*clamp*/ true);
  CHECK_EQ(p.cut_offset, std::int64_t{464});
}

// ---- rebaseCursor ----

TEST(rebase_no_drop_is_identity) {
  CHECK_EQ(rebaseCursor(300, 0, kHeader), std::int64_t{300});
}

TEST(rebase_shifts_by_dropped) {
  // Dropped 200 bytes; a cursor at 364 (start of r3 pre-trim) → 164.
  CHECK_EQ(rebaseCursor(364, 200, kHeader), std::int64_t{164});
}

TEST(rebase_inside_dropped_region_snaps_to_header) {
  // A cursor pointing into the dropped head lands on the new head.
  CHECK_EQ(rebaseCursor(164, 200, kHeader), kHeader);
}

TEST(rebase_exactly_at_cut_lands_on_header) {
  // cursor == header + dropped → exactly the new head.
  CHECK_EQ(rebaseCursor(kHeader + 200, 200, kHeader), kHeader);
}
