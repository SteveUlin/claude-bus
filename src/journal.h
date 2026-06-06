#pragma once

// A durable, append-only log of opaque byte records. The caller owns what
// the payload bytes mean; this layer stores and returns them verbatim and
// never interprets them, so one log serves any record shape. One file holds
// one log.
//
// On-disk wire — v6:
//
//   file header (64 bytes):
//     "BUS\0"               4
//     version (u32 LE)      4    — value 6
//     reserved              56   — forward-compat tail
//
//   record:
//     crc32c      (u32 LE)  4    CRC32C over [front record_len .. trailer],
//                                i.e. everything AFTER the crc
//     record_len  (u64 LE)  8    FRONT total record size incl crc + this +
//                                payload + trailer
//     append_ms   (u64 LE)  8    when the log received the bytes
//     seq         (u16 LE)  2    per-ms monotonic counter
//     payload     (bytes)        OPAQUE — the encoded envelope; length =
//                                record_len - overhead (30); the kernel
//                                never inspects it
//     record_len  (u64 LE)  8    TRAILER == front record_len; reverse-
//                                walk back-pointer + torn-frame check
//
//   Min record_len = crc(4) + front_len(8) + append_ms(8) + seq(2) +
//                    payload(0) + trailer(8) = 30.
//
// id format: "{append_ms:013}-{seq:04x}". The per-millisecond seq makes the
// pair unique, so the id carries no writer identity.
//
// On read, the CRC is verified before fields are parsed: a mismatch means
// a corrupt/torn record buried mid-log, so parsing stops there (truncate
// at first bad == logical EOF), preserving the refuse-torn-tail invariant.
//
// A single writer appends, so reads need no lock and the pre-write size
// capture in append() is race-free.

#include "types.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

constexpr std::uint32_t kJournalFormatVersion = 6;
constexpr std::size_t kJournalHeaderBytes = 64;
// Soft runaway-protection cap. Single broker = no concurrent-writer
// interleave, so PIPE_BUF is no longer the binding constraint. 1 MiB
// is roomy for any human-typed prompt while still catching a runaway
// producer that would balloon parse memory and slow restart replay.
constexpr std::size_t kJournalMaxRecordBytes = 1 << 20;  // 1 MiB

// Write-durability for an append. Buffered = today's single O_APPEND write
// (no sync); Synced = fdatasync the file (and, on file creation, fsync the
// parent dir) before returning so the record survives a crash/power-loss.
enum class Durability : std::uint8_t { Buffered = 0, Synced = 1 };

struct Record;

// Open / lazily-create a journal at the given path. Reads on a fresh
// path return empty result sets.
//
// state_root and name are needed for the cursor-store API (consumerCursor,
// ack, lastAckedId). They may be left empty when only the raw I/O methods
// (append, peek, dump, tail, trimHead) are used.
class Journal {
 public:
  // Opaque position handle for a consumer's read head in a journal. Carries
  // no public arithmetic — the kernel (Journal) owns all offset math, cursor
  // files, and the ack/dedup invariant. Callers hold a Cursor value, persist
  // it via toToken/fromToken, and pass it back to Journal methods.
  //
  // Default-constructed Cursor == start-of-log (before the first record).
  // Comparison (<=>) is valid so callers can assert monotonicity without
  // doing arithmetic (e.g. assert(new_cursor > old_cursor)).
  //
  // journal_tag_ binds a cursor to the journal that issued it (FNV-1a of
  // the journal name). Tag 0 = unbound (unnamed journal / start-of-log). The tag
  // is stamped whenever a NAMED journal hands out a cursor, and is persisted
  // in toToken as "<offset>:<tag>". Journal::ack crashes (fatal) when a
  // cursor's non-zero tag doesn't match the journal's own tag — cross-journal
  // acks are programmer errors, not runtime conditions.
  //
  // operator<=> / operator== compare only the offset (offset_): cursors from
  // the same journal are comparable by position, and cross-journal ordering
  // is never relied on.
  class Cursor {
   public:
    Cursor() = default;
    auto operator<=>(const Cursor& o) const { return offset_ <=> o.offset_; }
    auto operator==(const Cursor& o) const -> bool {
      return offset_ == o.offset_;
    }

    // Serialize to "<offset>:<tag>" for JSON persistence.
    // The token is opaque — never parse it outside Journal.
    auto toToken() const -> std::string;

    // Deserialize a token produced by toToken. Defaults to start-of-log on
    // an empty or malformed token.
    static auto fromToken(std::string_view token) -> Cursor;

   private:
    explicit Cursor(std::int64_t v) : offset_{v} {}
    Cursor(std::int64_t v, std::uint64_t tag) : offset_{v}, journal_tag_{tag} {}
    std::int64_t offset_{0};
    std::uint64_t journal_tag_{0};
    // Enclosing class accesses private constructors and members to mint
    // tagged cursors. The external friend for parseFrom is eliminated (the
    // whole point of nesting: parseFrom is now a private Journal static).
    friend class Journal;
  };

  explicit Journal(std::string path);

  // Construct with state_root + name for the full cursor-store API.
  Journal(std::string state_root, std::string name);

  // Append one record. Returns the record's id.
  auto append(std::span<const std::byte> payload,
              Durability durability = Durability::Buffered)
      -> bus::Result<std::string>;

  // Read all records at or past `from`, optionally capped at `limit`.
  // Cursor not modified. Pass Cursor{} to start from the beginning.
  auto peek(Cursor from, std::size_t limit = SIZE_MAX) const
      -> bus::Result<std::vector<Record>>;

  // Read all records currently in the log.
  auto dump() const -> bus::Result<std::vector<Record>>;

  // Return the most recent n records in CHRONOLOGICAL order
  // (oldest-of-the-window first), fetched by walking UP from EOF.
  // On a torn tail, falls back to forward dump + take-last-n.
  // On any mid-walk corruption, stops (records older than the gap
  // are dropped, consistent with forward's truncate-at-first-bad).
  auto tail(std::size_t n) const -> bus::Result<std::vector<Record>>;

  // Drop the head of the log up to `cut` (a record boundary Cursor), preserving
  // the file header and splicing the surviving tail down so it begins right
  // after the header. Atomic (tmp + rename). Returns the number of bytes
  // dropped on success, or 0 when the cut is a no-op / out of range (≤ header
  // or ≥ the file size). After a successful trim, persisted consumer cursors
  // for this journal are rebased automatically so they still point at the same
  // logical record (the kernel owns that rebase — callers never compute the
  // shift).
  auto trimHead(Cursor cut) -> std::int64_t;

  // Plan and execute a head trim by retention policy. Reads all records,
  // calls planTrim with the given parameters, then trims and rebases in one
  // step. Returns the bytes dropped (0 = nothing trimmed). The caller passes
  // only policy knobs; all offset arithmetic stays inside the kernel.
  //
  // BAD CALL: calling on a journal without state_root+name crashes (needed
  // for cursor rebase + minConsumerOffset).
  auto trimByPolicy(std::int64_t retention_ms, std::int64_t max_bytes,
                    std::int64_t now_ms, bool clamp_to_cursor) -> std::int64_t;

  // Path to the journal file.
  auto path() const -> const std::string& { return path_; }

  // ── Cursor-store API (requires state_root + name) ──────────────────────
  //
  // Read the persisted cursor for `consumer` ("" = default). Returns the
  // start-of-log Cursor when no cursor file exists yet.
  //
  // BAD CALL: calling on a journal built without state_root+name crashes.
  auto consumerCursor(std::string_view consumer = "") const -> Cursor;

  // Advance the consumer cursor to `target` — forward-only; a backward or
  // equal target is a no-op (at-least-once invariant). Also stamps the
  // last-acked id when `id` is non-empty. Returns true iff it actually wrote.
  //
  // BAD CALL: calling on a journal without state_root+name crashes.
  // BAD CALL: target's tag is non-zero and != this journal's tag crashes
  //           (cross-journal cursor — always a programmer error).
  auto ack(std::string_view consumer, Cursor target,
           std::string_view id = "") -> bool;

  // Read the last-acked record id for `consumer` (the dedup floor).
  // Empty string when none has been stamped.
  //
  // BAD CALL: calling on a journal without state_root+name crashes.
  auto lastAckedId(std::string_view consumer = "") const -> std::string;

  // ── Opaque persistence tokens (forwarded to Cursor) ───────────────────
  //
  // Serialize a Cursor to a string token for JSON persistence (in-flight
  // files). The token is an opaque string — never parse it.
  static auto cursorToToken(Cursor c) -> std::string;

  // Deserialize a token produced by cursorToToken. Defaults to
  // start-of-log on an empty or malformed token.
  static auto cursorFromToken(std::string_view token) -> Cursor;

  // ── Trim-rebase for in-flight Cursors ─────────────────────────────────
  //
  // After trimHead drops `dropped_bytes` from the head, rebase a Cursor
  // that was recorded before the trim. A cursor inside the dropped region
  // snaps to the new head (header position). Callers (delivery) use this
  // for in-flight cursor_after values; the kernel uses it for persisted
  // consumer cursors inside trimHead.
  auto rebaseCursor(Cursor c, std::int64_t dropped_bytes) const -> Cursor;

  // ── Kernel accessor: cursor after a record ────────────────────────────
  //
  // Derive the Cursor that advances past `r` — equivalent to the
  // next-record start. The kernel owns this arithmetic so callers never
  // compute offsets directly.
  static auto cursorAfter(const Record& r) -> Cursor;

  // Minimum persisted consumer offset across all consumers for this
  // journal. Returns 0 when no cursor files exist (the "nobody has read"
  // floor). Used by the retention planner to enforce the at-least-once
  // clamp.
  //
  // BAD CALL: calling on a journal without state_root+name crashes.
  auto minConsumerOffset() const -> std::int64_t;

  // ── Test entry point (narrowly exposed; production code uses peek/dump) ──
  //
  // Parse records from a raw journal buffer. Equivalent to the private wire
  // parser, but accessible for hand-built buffer tests (CRC corruption,
  // torn tail, corrupt record_len). Stops cleanly at the last whole record.
  // start_offset is clamped to kJournalHeaderBytes.
  static auto parseForTest(std::span<const std::byte> buf,
                           std::int64_t start_offset,
                           std::size_t limit = SIZE_MAX) -> std::vector<Record>;

  // True iff the journal file at `path` carries a version header that
  // differs from kJournalFormatVersion. Lenient: a file that can't be
  // opened or is too short is NOT stale (returns false).
  static auto isStaleVersion(const std::string& path) -> bool;

 private:
  std::string path_;
  std::string state_root_;  // empty when constructed without cursor-store args
  std::string name_;        // empty when constructed without cursor-store args

  auto cursorFilePath(std::string_view consumer) const -> std::string;
  auto lastIdFilePath(std::string_view consumer) const -> std::string;

  // Extract the raw byte offset from a Cursor. Private — only the kernel
  // (retention planning, trimHead, trimByPolicy, peek) uses this.
  static auto toOffset(Cursor c) -> std::int64_t;

  // Internal wire parser — private; production entry points are peek/dump/tail.
  static auto ParseFromBuffer(std::span<const std::byte> buf,
                              std::int64_t start_offset,
                              std::size_t limit,
                              std::uint64_t tag) -> std::vector<Record>;
};

struct Record {
  std::string id;                  // "{append_ms:013}-{seq:04x}"
  std::int64_t append_ms{};
  std::vector<std::byte> payload;  // opaque bytes — the encoded envelope

  // Opaque position of the START of this record. Hold and pass back to
  // Journal methods — never parse. Use Journal::cursorAfter(*this) to
  // obtain the cursor past this record.
  Journal::Cursor position;
};

// Read / write a raw u64 (little-endian) to a small file atomically.
// For values that are NOT consumer cursors — e.g. the continuity.ms
// wall-clock stamp. Using the cursor-store API for non-cursor values
// would conflate two distinct namespaces.
auto readU64File(const std::string& path) -> std::int64_t;
auto writeU64File(const std::string& path, std::int64_t v) -> bool;

}  // namespace bus
