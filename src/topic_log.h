#pragma once

// Per-topic atomic-append log. One file per topic at
// $STATE/topics/<name>.log. Each topic is a stream of typed records;
// cursors live in $STATE/cursors/<topic>/<consumer>.cursor so a topic
// can have many independent consumers (work-queue / pubsub).
//
// On-disk wire — v4:
//
//   file header (64 bytes):
//     "BUS\0"               4
//     version (u32 LE)      4    — value 4
//     reserved              56   — created_ms etc. (forward-compat tail)
//
//   record:
//     record_len (u64 LE)   8    — total record size, header included
//     sent_ms    (u64 LE)   8
//     rand_tag   (u16 LE)   2
//     ttl_ms     (u32 LE)   4    — 0 = no expiry
//     deliver_when (u8)     1    — 0 immediate, 1 idle
//     sender_len  (u8)      1
//     sender     (bytes)
//     protocol_len (u8)     1
//     protocol   (bytes)
//     correlation (u128)    16   — RPC pairing; 0 = none
//     body_len    (u32 LE)  4
//     body       (bytes)
//
// The broker is the only writer to topic logs (every producer goes
// through it via RPC), so concurrent-writer interleave isn't a risk
// and we don't depend on PIPE_BUF atomicity. The cap below is a
// soft runaway-protection limit, not a correctness one.

#include "types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus::topic {

constexpr std::uint32_t kFormatVersion = 4;
constexpr std::size_t kFileHeaderBytes = 64;
// Soft runaway-protection cap. Single broker = no concurrent-writer
// interleave, so PIPE_BUF is no longer the binding constraint. 1 MiB
// is roomy for any human-typed prompt (~100 KiB ceiling in practice)
// while still catching a runaway producer that would balloon parse
// memory and slow the restart replay.
constexpr std::size_t kMaxRecordBytes = 1 << 20;  // 1 MiB

struct SendOpts {
  std::uint32_t ttl_ms{0};
  std::uint8_t deliver_when{0};  // 0=immediate, 1=idle
  std::string protocol{"text"};
  std::array<std::uint8_t, 16> correlation{};
};

struct Message {
  std::string id;            // "{sent_ms:013}-{sender}-{rand:04x}"
  std::int64_t sent_ms{};
  std::string sender;
  std::uint32_t ttl_ms{};
  std::uint8_t deliver_when{};
  std::string protocol;
  std::array<std::uint8_t, 16> correlation{};
  std::string body;

  // Byte offsets in the file (NOT relative to message region):
  // `offset` is where the record's `record_len` field starts;
  // `next_offset` is `offset + record_len` (where the next record
  // would begin). Consumers persist these as cursor values.
  std::int64_t offset{};
  std::int64_t next_offset{};
};


// Open / lazily-create a topic log at the given path. Reads on a fresh
// path return empty result sets.
class TopicLog {
 public:
  explicit TopicLog(std::string path);

  // Append one record. `sender` is stamped on the record (e.g., the
  // agent id of the caller). Returns the record's id.
  auto append(std::string_view sender, std::string_view body,
              const SendOpts& opts) -> bus::Result<std::string>;

  // Read all records at or past `start_offset`, optionally capped at
  // `limit`. Cursor not modified.
  auto peek(std::int64_t start_offset, std::size_t limit = SIZE_MAX) const
      -> bus::Result<std::vector<Message>>;

  // Read all records currently in the log.
  auto dump() const -> bus::Result<std::vector<Message>>;

  // Path to the topic log file.
  auto path() const -> const std::string& { return path_; }

 private:
  std::string path_;
};

// Parse records out of a raw topic-log buffer, starting at the given
// byte offset (clamped to the file header), capped at `limit`. Stops
// cleanly at the last whole record — a truncated tail record is left
// unparsed (the same refuse-torn-tail invariant the append-log reader
// relies on). Exposed for direct table-testing with hand-built buffers;
// TopicLog::peek is the file-backed entry point.
auto parseFrom(std::span<const std::byte> buf, std::int64_t start_offset,
               std::size_t limit = SIZE_MAX) -> std::vector<Message>;

// Cursor helpers — read / write u64 byte offsets to a small file
// atomically. The cursor file is created on first write.
auto readCursor(const std::string& path) -> std::int64_t;
auto writeCursor(const std::string& path, std::int64_t offset) -> bool;

// Compute the cursor path for a (topic, consumer) pair, rooted at
// $STATE/cursors/. Consumer "" maps to "_default".
auto cursorPath(std::string_view state_root, std::string_view topic,
                std::string_view consumer) -> std::string;

}  // namespace bus::topic
