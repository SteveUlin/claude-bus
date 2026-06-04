#pragma once

// Pure retention/trim decision logic for topic logs (roadmap D2) — the
// "where do I cut the head?" math, factored out of the file I/O so it can
// be table-tested with hand-built record metadata. The file-backed trim +
// cursor rebase live in delivery.cpp (maybeTrimLogs); they call planTrim
// and rebaseCursor.

#include <cstdint>
#include <vector>

namespace bus::retention {

// The minimal per-record metadata the cut decision needs. `offset` is the
// byte position where the record starts; `next_offset` is just past it;
// records are in file (append / time) order.
struct RecordMeta {
  std::int64_t offset{};
  std::int64_t next_offset{};
  std::int64_t sent_ms{};
};

struct TrimPlan {
  // Keep records at/after this offset. Equals header_bytes when nothing is
  // trimmed. Always lands on a record boundary.
  std::int64_t cut_offset{};
  // cut_offset - header_bytes; the count of head bytes the trim removes.
  std::int64_t dropped_bytes{};
};

// Decide where to cut a topic log's head.
//
//   records          in ascending-offset (append) order; may be empty.
//   now_ms           current wall clock in ms.
//   retention_ms     drop leading records with sent_ms+retention_ms < now;
//                    0 disables the age rule.
//   max_bytes        drop oldest records until the kept span (EOF - cut)
//                    is <= max_bytes; 0 disables the size rule.
//   header_bytes     the file header size (records start here); also the
//                    floor cut_offset can take.
//   min_cursor       the smallest consumer cursor for this topic (the
//                    "everyone has read up to here" floor).
//   clamp_to_cursor  true for delivery-guaranteed kinds (agent-inbox /
//                    tui-commands): never cut past min_cursor, so an
//                    undelivered/in-flight record is never dropped. false
//                    for fire-and-forget kinds (audit / pubsub / work-queue
//                    / blackboard): expiry drops regardless of consumption.
inline auto planTrim(const std::vector<RecordMeta>& records,
                     std::int64_t now_ms, std::int64_t retention_ms,
                     std::int64_t max_bytes, std::int64_t header_bytes,
                     std::int64_t min_cursor,
                     bool clamp_to_cursor) -> TrimPlan {
  TrimPlan plan{header_bytes, 0};
  if (records.empty()) return plan;

  std::int64_t cut = header_bytes;

  // Age rule: walk leading expired records. They're time-ordered, so the
  // first live record ends the scan.
  if (retention_ms > 0) {
    for (const auto& r : records) {
      if (r.sent_ms + retention_ms < now_ms) {
        cut = r.next_offset;
      } else {
        break;
      }
    }
  }

  // Size rule: keep the smallest tail whose span is within the cap. The
  // earliest record r with (EOF - r.offset) <= max_bytes is the oldest we
  // can keep; drop everything before it.
  if (max_bytes > 0) {
    const std::int64_t eof = records.back().next_offset;
    for (const auto& r : records) {
      if (eof - r.offset <= max_bytes) {
        if (r.offset > cut) cut = r.offset;
        break;
      }
    }
  }

  // Delivery-guarantee clamp: only drop records every consumer has passed.
  if (clamp_to_cursor && cut > min_cursor) cut = min_cursor;
  if (cut < header_bytes) cut = header_bytes;

  plan.cut_offset = cut;
  plan.dropped_bytes = cut - header_bytes;
  return plan;
}

// Rebase one absolute byte cursor after `dropped_bytes` were removed from
// the head. A cursor inside the dropped region snaps to the new head.
inline auto rebaseCursor(std::int64_t cursor, std::int64_t dropped_bytes,
                         std::int64_t header_bytes) -> std::int64_t {
  if (dropped_bytes <= 0) return cursor;
  const std::int64_t rebased = cursor - dropped_bytes;
  return rebased < header_bytes ? header_bytes : rebased;
}

}  // namespace bus::retention
