#pragma once

#include "types.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

// Forward declaration so CursorStore can be a friend of Journal::Cursor
// without pulling in cursor_store.h (which includes journal.h).
class CursorStore;

constexpr std::uint32_t kJournalFormatVersion = 7;
constexpr std::size_t kJournalHeaderBytes = 64;
constexpr std::size_t kJournalMaxRecordBytes = 1 << 20;  // 1 MiB

// Write-durability for an append.
enum class Durability : std::uint8_t { Buffered = 0, Synced = 1 };

struct Record;

// A Journal is a durable append only log of opaque byte records. The caller
// owns what the payload bytes mean.
//
// On-disk wire — v7:
//
//   file header (64 bytes):
//     "BUS\0"               4    - magic bytes for filetype
//     version (u32 LE)      4    — value 7
//     uuid  (16 bytes)      16   — random per-file identity; source of the
//                                  cursor binding tag (UUID-derived FNV-1a)
//     reserved              40   — forward-compat tail
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
// TODO: updated id format
// "{human readable alpha num (is meaningless for the api)}-{uuid}-{append_ms:13}-{seq:04x}}"
// TODO: why time based ids? Consider keeping append_ms for debugging only
//       maybe store the number of records at the end of the file and just
//       have a monotonically increasing id? Simplifies referencing records
//       --- 
//       maybe an id should have a byte offset for direct reads?
// TODO: Isnt an id just a cursor? Can we dedupe
//
// On read, the CRC is verified before fields are parsed: a mismatch means
// a corrupt/torn record buried mid-log, so parsing stops there (truncate
// at first bad == logical EOF), preserving the refuse-torn-tail invariant.
// The read methods report where they stopped via the `truncated_at` out-param,
// so a caller distinguishes a clean end-of-log from a corruption-truncated read
// and picks its own recovery.

// Open / lazily-create a journal at the given path. Reads on a fresh
// path return empty result sets.
//
// TODO: no lazy creation (lazy actions are for higher level apis if needed)
//
// Journal is path-only: it owns byte ops (append, peek, dump, tail) only.
// Per-consumer cursor persistence lives in CursorStore (cursor_store.h).
//
// Thought: a journal should support multiple cursors
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
  // the journal's on-disk UUID). Tag 0 = unbound (file not yet created /
  // path-only journal). The tag is stamped when a journal that has an
  // on-disk UUID hands out a cursor, and is persisted in toToken as
  // "<offset>:<tag>". Journal::ack crashes (fatal) when a cursor's non-zero
  // tag doesn't match the journal's own tag — cross-journal acks are
  // programmer errors, not runtime conditions.
  //
  // operator<=> / operator== compare only the offset (offset_): cursors from
  // the same journal are comparable by position, and cross-journal ordering
  // is never relied on.
  //
  // TODO: Should cursors contain logic for reading to and and from files?
  //       Nah, probably should be free functions that do this.
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
    // Cursor's constructors are private so a position value can't be forged
    // from a raw integer outside this unit. Journal is the primary factory —
    // it mints tagged Cursors in ParseFromBuffer and cursorAfter.
    // CursorStore is a second sanctioned factory: it mints Cursors in
    // consumerCursor (stamping the journal's tag) and reads offset_/journal_tag_
    // in ack (for persistence + the cross-journal guard). Cursors remain
    // un-forgeable by arbitrary code.
    friend class Journal;
    friend class CursorStore;
  };

  explicit Journal(std::string path);

  // Append one record. Returns the record's id.
  auto append(std::span<const std::byte> payload,
              Durability durability = Durability::Buffered)
      -> bus::Result<std::string>;

  // Read all records at or past `from`, optionally capped at `limit`.
  // Cursor not modified. Pass Cursor{} to start from the beginning.
  // *truncated_at is set to the byte offset of the first bad/incomplete record
  // on parse failure, or -1 on a clean end-of-log or limit-bounded stop.
  auto peek(Cursor from, std::size_t limit = SIZE_MAX,
            std::int64_t* truncated_at = nullptr) const
      -> bus::Result<std::vector<Record>>;

  // Read all records currently in the log.
  // *truncated_at is set to the byte offset of the first bad/incomplete record
  // on parse failure, or -1 on a clean end-of-log or limit-bounded stop.
  auto dump(std::int64_t* truncated_at = nullptr) const
      -> bus::Result<std::vector<Record>>;

  // Return the most recent n records in CHRONOLOGICAL order
  // (oldest-of-the-window first), fetched by walking UP from EOF.
  // On a torn tail, falls back to forward dump + take-last-n.
  // On any mid-walk corruption, stops (records older than the gap
  // are dropped, consistent with forward's truncate-at-first-bad).
  // *truncated_at is set to the byte offset of the first bad/incomplete record
  // on parse failure, or -1 on a clean end-of-log or limit-bounded stop.
  auto tail(std::size_t n, std::int64_t* truncated_at = nullptr) const
      -> bus::Result<std::vector<Record>>;

  // Path to the journal file.
  auto path() const -> const std::string& { return path_; }

  // ── Journal identity tag ───────────────────────────────────────────────
  //
  // Read the UUID at kUuidOffset from the file at `path` and derive the
  // FNV-1a tag. Returns 0 when the file doesn't exist or is too short
  // (unbound). This is the public, uncached static used by CursorStore
  // to stamp/verify cursors without holding a Journal instance.
  //
  // The instance tag() delegates to this and caches the first non-zero result.
  static auto tagOf(const std::string& path) -> std::uint64_t;

  // ── Opaque persistence tokens (forwarded to Cursor) ───────────────────
  //
  // Serialize a Cursor to a string token for JSON persistence (in-flight
  // files). The token is an opaque string — never parse it.
  static auto cursorToToken(Cursor c) -> std::string;

  // Deserialize a token produced by cursorToToken. Defaults to
  // start-of-log on an empty or malformed token.
  static auto cursorFromToken(std::string_view token) -> Cursor;

  // ── Kernel accessor: cursor after a record ────────────────────────────
  //
  // Derive the Cursor that advances past `r` — equivalent to the
  // next-record start. The kernel owns this arithmetic so callers never
  // compute offsets directly.
  static auto cursorAfter(const Record& r) -> Cursor;

  // ── Test entry point ──────────────────────────────────────────────────────
  //
  // Exposes the private wire parser for hand-built buffer tests (CRC
  // corruption, torn tail, oversized record_len). Kept off the public
  // surface so the parser implementation can change without affecting
  // production callers, who always go through peek / dump / tail.
  // start_offset is clamped to kJournalHeaderBytes.
  // *truncated_at is set to the byte offset of the first bad/incomplete record
  // on parse failure, or -1 on a clean end-of-log or limit-bounded stop.
  static auto parseForTest(std::span<const std::byte> buf,
                           std::int64_t start_offset,
                           std::size_t limit = SIZE_MAX,
                           std::int64_t* truncated_at = nullptr)
      -> std::vector<Record>;

  // True iff the journal file at `path` carries a version header that
  // differs from kJournalFormatVersion. Lenient: a file that can't be
  // opened or is too short is NOT stale (returns false).
  static auto isStaleVersion(const std::string& path) -> bool;

 private:
  std::string path_;

  // UUID-derived tag: delegates to tagOf(path_) and caches the first non-zero
  // result. Returns 0 when the file doesn't exist yet or is too short.
  mutable std::uint64_t cached_tag_{0};
  auto tag() const -> std::uint64_t;

  // Extract the raw byte offset from a Cursor. Private — the kernel owns all
  // offset arithmetic so callers hold Cursors opaquely. Used by the read path
  // (peek, consumerCursor) and cursorAfter to compute record positions.
  static auto toOffset(Cursor c) -> std::int64_t;

  // Wire parser shared by peek, dump, tail, and parseForTest. Kept private
  // so the on-disk layout is owned entirely by Journal — no caller can bypass
  // the CRC/torn-record logic or produce a Record with an unvalidated offset.
  // *truncated_at receives the byte offset of the first bad record on failure,
  // or -1 on clean end-of-log / limit-bounded stop. Ignored when nullptr.
  static auto ParseFromBuffer(std::span<const std::byte> buf,
                              std::int64_t start_offset,
                              std::size_t limit,
                              std::uint64_t tag,
                              std::int64_t* truncated_at = nullptr)
      -> std::vector<Record>;
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
