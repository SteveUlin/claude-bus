#include "journal.h"

#include "crc32c.h"

#include <cassert>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace bus {

// ── Fatal error (programmer error; always crashes) ────────────────────────

[[noreturn]] static auto fatal(std::string_view msg) -> void {
  std::string s = "bus::Journal fatal: ";
  s += msg;
  s += '\n';
  // Intentionally ignore the return value: we are about to abort() and a
  // write failure just means stderr is closed — nothing useful to do.
  [[maybe_unused]] auto _ = ::write(STDERR_FILENO, s.data(), s.size());
  std::abort();
}

namespace {

constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kUuidOffset = 8;
constexpr std::size_t kUuidBytes = 16;
// Fixed framing per record: crc(4) + front_len(8) + append_ms(8) +
// seq(2) + trailer(8).  Payload length = record_len - kRecordOverhead.
constexpr std::uint64_t kRecordOverhead = 30;

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_{fd} {}
  ~Fd() { reset(); }
  Fd(const Fd&) = delete;
  auto operator=(const Fd&) -> Fd& = delete;
  Fd(Fd&& o) noexcept : fd_{o.fd_} { o.fd_ = -1; }
  auto operator=(Fd&& o) noexcept -> Fd& {
    if (this != &o) {
      reset();
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }
  auto get() const -> int { return fd_; }
  auto valid() const -> bool { return fd_ >= 0; }
  auto reset() -> void {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_{-1};
};

using bus::Error;
using bus::Result;

auto err(std::string what) -> Error { return Error{std::move(what)}; }

auto errFromErrno(int e, std::string_view what) -> Error {
  return err(std::format("{}: {}", what, std::strerror(e)));
}

auto nowMs() -> std::int64_t {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

// A single writer appends, so no lock is needed. One counter makes seq
// unique within a millisecond, so the {ms}-{seq} pair cannot collide.
// Clock-rewind safe: when the clock does not advance, hold the timestamp
// and bump seq rather than emit a smaller one.
auto nextSeq(std::int64_t& append_ms) -> std::uint16_t {
  static std::int64_t last_ms = -1;
  static std::uint16_t seq = 0;
  if (append_ms <= last_ms) {
    append_ms = last_ms;
    if (++seq == 0) {
      ++last_ms;
      ++append_ms;
    }
  } else {
    last_ms = append_ms;
    seq = 0;
  }
  return seq;
}

auto putU16(std::vector<std::byte>& b, std::uint16_t v) -> void {
  b.push_back(std::byte(v & 0xFF));
  b.push_back(std::byte((v >> 8) & 0xFF));
}
auto putU32(std::vector<std::byte>& b, std::uint32_t v) -> void {
  for (int i = 0; i < 4; ++i) b.push_back(std::byte((v >> (i * 8)) & 0xFF));
}
auto putU64(std::vector<std::byte>& b, std::uint64_t v) -> void {
  for (int i = 0; i < 8; ++i) b.push_back(std::byte((v >> (i * 8)) & 0xFF));
}
auto putBytes(std::vector<std::byte>& b,
              std::span<const std::byte> s) -> void {
  b.insert(b.end(), s.begin(), s.end());
}
auto patchU32(std::span<std::byte> b, std::size_t at,
              std::uint32_t v) -> void {
  for (int i = 0; i < 4; ++i) b[at + i] = std::byte((v >> (i * 8)) & 0xFF);
}
auto patchU64(std::span<std::byte> b, std::size_t at,
              std::uint64_t v) -> void {
  for (int i = 0; i < 8; ++i) b[at + i] = std::byte((v >> (i * 8)) & 0xFF);
}

auto getU16(std::span<const std::byte> b) -> std::uint16_t {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(b[0])) |
         (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(b[1])) << 8);
}
auto getU32(std::span<const std::byte> b) -> std::uint32_t {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[i]))
         << (i * 8);
  }
  return v;
}
auto getU64(std::span<const std::byte> b) -> std::uint64_t {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b[i]))
         << (i * 8);
  }
  return v;
}

// FNV-1a 64-bit over a 16-byte UUID. Stable across processes (no std::hash).
// Tag 0 is reserved = "unbound"; a UUID that hashes to 0 (vanishingly unlikely)
// is remapped to 1 so it never collides with the unbound sentinel.
auto journalTagFromUuid(std::span<const std::byte> uuid) -> std::uint64_t {
  constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t kFnvPrime  =        1'099'511'628'211ULL;
  std::uint64_t h = kFnvOffset;
  for (const auto b : uuid) {
    h ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
    h *= kFnvPrime;
  }
  return h == 0 ? 1 : h;
}

auto makeFileHeader() -> std::array<std::byte, kJournalHeaderBytes> {
  std::array<std::byte, kJournalHeaderBytes> h{};
  h[0] = std::byte{'B'};
  h[1] = std::byte{'U'};
  h[2] = std::byte{'S'};
  h[3] = std::byte{0};
  const std::uint32_t v = kJournalFormatVersion;
  for (int i = 0; i < 4; ++i) {
    h[kVersionOffset + i] = std::byte((v >> (i * 8)) & 0xFF);
  }
  // Random 16-byte UUID at kUuidOffset (bytes 8..23). Each file gets a unique
  // identity; the UUID is the source of the cursor binding tag.
  std::random_device rd;
  const std::uint64_t u0 = (static_cast<std::uint64_t>(rd()) << 32) | rd();
  const std::uint64_t u1 = (static_cast<std::uint64_t>(rd()) << 32) | rd();
  for (int i = 0; i < 8; ++i) {
    h[kUuidOffset + i]     = std::byte((u0 >> (i * 8)) & 0xFF);
    h[kUuidOffset + 8 + i] = std::byte((u1 >> (i * 8)) & 0xFF);
  }
  return h;
}

// Returns true iff this call CREATED the file (so the Synced append path
// knows it must fsync the parent dir to make the new dirent durable).
auto ensureHeader(const std::string& path) -> Result<bool> {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) {
    Fd guard{fd};
    const auto h = makeFileHeader();
    if (::pwrite(fd, h.data(), kJournalHeaderBytes, 0) !=
        static_cast<ssize_t>(kJournalHeaderBytes)) {
      const int e = errno;
      ::unlink(path.c_str());
      return std::unexpected{errFromErrno(e, "write header")};
    }
    return true;
  }
  if (errno != EEXIST) {
    return std::unexpected{errFromErrno(errno, "init journal")};
  }
  return false;
}

auto readAll(const std::string& path) -> Result<std::vector<std::byte>> {
  Fd fd{::open(path.c_str(), O_RDONLY)};
  if (!fd.valid()) {
    if (errno == ENOENT) return std::vector<std::byte>{};
    return std::unexpected{errFromErrno(errno, "open journal")};
  }
  struct stat st;
  if (::fstat(fd.get(), &st) != 0) {
    return std::unexpected{errFromErrno(errno, "fstat")};
  }
  std::vector<std::byte> buf(static_cast<std::size_t>(st.st_size));
  if (buf.empty()) return buf;
  if (::pread(fd.get(), buf.data(), buf.size(), 0) !=
      static_cast<ssize_t>(buf.size())) {
    return std::unexpected{errFromErrno(errno, "pread journal")};
  }
  if (buf.size() >= kJournalHeaderBytes) {
    const auto v = getU32({buf.data() + kVersionOffset, 4});
    if (v != kJournalFormatVersion) {
      return std::unexpected{err(std::format(
          "journal {} has format v{}, runtime expects v{}; wipe and retry",
          path, v, kJournalFormatVersion))};
    }
  }
  return buf;
}

// Cursor path for a (journal, consumer) pair, rooted at $STATE/cursors/.
// Consumer "" maps to "_default".
auto cursorPathImpl(std::string_view state_root, std::string_view name,
                    std::string_view consumer) -> std::string {
  std::string c{consumer};
  if (c.empty()) c = "_default";
  return std::string{state_root} + "/cursors/" + std::string{name} + "/" +
         c + ".cursor";
}

auto readCursorImpl(const std::string& path) -> std::int64_t {
  Fd fd{::open(path.c_str(), O_RDONLY)};
  if (!fd.valid()) return 0;
  std::array<std::byte, 8> buf{};
  const auto n = ::read(fd.get(), buf.data(), buf.size());
  if (n != 8) return 0;
  return static_cast<std::int64_t>(getU64({buf.data(), 8}));
}

auto writeCursorImpl(const std::string& path, std::int64_t offset) -> bool {
  std::error_code ec;
  fs::create_directories(fs::path{path}.parent_path(), ec);
  if (ec) return false;
  const std::string tmp = path + ".tmp";
  Fd fd{::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
  if (!fd.valid()) return false;
  std::array<std::byte, 8> buf{};
  const auto v = static_cast<std::uint64_t>(offset);
  for (int i = 0; i < 8; ++i) buf[i] = std::byte((v >> (i * 8)) & 0xFF);
  if (::write(fd.get(), buf.data(), buf.size()) != 8) return false;
  fd.reset();
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

auto advanceCursorMonotonicImpl(const std::string& path,
                                std::int64_t target) -> bool {
  if (readCursorImpl(path) >= target) return false;
  return writeCursorImpl(path, target);
}

auto lastIdPathImpl(const std::string& cursor_path) -> std::string {
  constexpr std::string_view kSuffix = ".cursor";
  if (cursor_path.size() >= kSuffix.size() &&
      cursor_path.compare(cursor_path.size() - kSuffix.size(),
                          kSuffix.size(), kSuffix) == 0) {
    return cursor_path.substr(0, cursor_path.size() - kSuffix.size()) +
           ".lastid";
  }
  return cursor_path + ".lastid";
}

auto readLastIdImpl(const std::string& path) -> std::string {
  Fd fd{::open(path.c_str(), O_RDONLY)};
  if (!fd.valid()) return {};
  std::array<char, 256> buf{};
  const auto n = ::read(fd.get(), buf.data(), buf.size());
  if (n <= 0) return {};
  std::string id(buf.data(), static_cast<std::size_t>(n));
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r' ||
                         id.back() == ' ')) {
    id.pop_back();
  }
  return id;
}

auto writeLastIdImpl(const std::string& path, const std::string& id) -> bool {
  std::error_code ec;
  fs::create_directories(fs::path{path}.parent_path(), ec);
  if (ec) return false;
  const std::string tmp = path + ".tmp";
  Fd fd{::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
  if (!fd.valid()) return false;
  const std::string line = id + "\n";
  if (::write(fd.get(), line.data(), line.size()) !=
      static_cast<ssize_t>(line.size())) {
    return false;
  }
  fd.reset();
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

// ── Cursor token serialization ─────────────────────────────────────────────

auto Journal::Cursor::toToken() const -> std::string {
  return std::to_string(offset_) + ":" + std::to_string(journal_tag_);
}

auto Journal::Cursor::fromToken(std::string_view token) -> Cursor {
  if (token.empty()) return Cursor{};
  try {
    const auto colon = token.find(':');
    // A token is "<offset>:<tag>" by construction; no colon means corrupt
    // persisted state — default to start-of-log (at-least-once re-evaluates).
    if (colon == std::string_view::npos) return Cursor{};
    const auto offset = static_cast<std::int64_t>(
        std::stoll(std::string{token.substr(0, colon)}));
    const auto tag = static_cast<std::uint64_t>(
        std::stoull(std::string{token.substr(colon + 1)}));
    return Cursor{offset, tag};
  } catch (...) {
    return Cursor{};
  }
}

// ── Journal::ParseFromBuffer (private wire parser) ─────────────────────────

auto Journal::ParseFromBuffer(std::span<const std::byte> buf,
                              std::int64_t start_offset,
                              std::size_t limit,
                              std::uint64_t tag,
                              std::int64_t* truncated_at) -> std::vector<Record> {
  std::vector<Record> out;
  if (buf.size() < kJournalHeaderBytes) {
    if (truncated_at) *truncated_at = -1;
    return out;
  }
  std::size_t pos =
      start_offset > static_cast<std::int64_t>(kJournalHeaderBytes)
          ? static_cast<std::size_t>(start_offset)
          : kJournalHeaderBytes;
  // v6 minimum record: kRecordOverhead = 30 (empty payload).
  constexpr std::uint64_t kMinRecordLen = kRecordOverhead;

  std::int64_t failure_offset = -1;  // set on any parse-failure break

  // Need crc(4) + record_len(8) before we can attempt a record.
  while (pos + 12 <= buf.size() && out.size() < limit) {
    const std::uint32_t crc = getU32({buf.data() + pos, 4});
    const std::uint64_t rec_len = getU64({buf.data() + pos + 4, 8});
    // Guard against overflow in pos + rec_len: if rec_len exceeds the
    // runaway-protection cap it can't be a real record.
    if (rec_len < kMinRecordLen || rec_len > kJournalMaxRecordBytes ||
        pos + rec_len > buf.size()) {
      failure_offset = static_cast<std::int64_t>(pos);
      break;
    }

    // Verify the CRC: covers [record_len .. trailer] (everything after the
    // crc field). A mismatch is a corrupt/torn record — stop here (truncate
    // at first bad == logical EOF).
    if (Crc32c({buf.data() + pos + 4,
                static_cast<std::size_t>(rec_len - 4)}) != crc) {
      failure_offset = static_cast<std::int64_t>(pos);
      break;
    }

    // All field reads below are safe: CRC passed over rec_len bytes starting
    // at pos+4, and pos + rec_len <= buf.size() was checked above.
    std::size_t p = pos + 12;  // past crc + front record_len
    const std::uint64_t append_ms = getU64({buf.data() + p, 8});
    p += 8;
    const std::uint16_t seq = getU16({buf.data() + p, 2});
    p += 2;
    // Payload length derived from record_len; no stored payload_len field.
    const std::uint64_t plen = rec_len - kRecordOverhead;
    std::vector<std::byte> payload(buf.data() + p, buf.data() + p + plen);
    p += plen;
    // Validate trailer == front record_len (torn-frame check; also inside CRC).
    const std::uint64_t trailer = getU64({buf.data() + p, 8});
    if (trailer != rec_len) {
      failure_offset = static_cast<std::int64_t>(pos);
      break;
    }

    // position = cursor at start of this record (offset of crc32c field).
    const Journal::Cursor pos_cursor{
        static_cast<std::int64_t>(pos), tag};
    out.push_back(Record{
        .id = std::format("{:013}-{:04x}", append_ms, seq),
        .append_ms = static_cast<std::int64_t>(append_ms),
        .payload = std::move(payload),
        .position = pos_cursor,
    });
    pos += rec_len;
  }

  if (truncated_at) *truncated_at = failure_offset;
  return out;
}

// ── Journal::parseForTest ──────────────────────────────────────────────────

// Delegates to the private wire parser with tag=0 (unbound). Tag 0 lets
// tests build arbitrary in-memory buffers without constructing a named
// Journal — the tag mismatch check only fires on non-zero tags.
auto Journal::parseForTest(std::span<const std::byte> buf,
                           std::int64_t start_offset,
                           std::size_t limit,
                           std::int64_t* truncated_at) -> std::vector<Record> {
  // Tag 0 = unbound (no named journal in test contexts).
  return ParseFromBuffer(buf, start_offset, limit, /*tag=*/0, truncated_at);
}

// ── Journal::isStaleVersion ────────────────────────────────────────────────

auto Journal::isStaleVersion(const std::string& path) -> bool {
  Fd fd{::open(path.c_str(), O_RDONLY)};
  if (!fd.valid()) return false;
  std::array<std::byte, kVersionOffset + 4> hdr{};
  if (::pread(fd.get(), hdr.data(), hdr.size(), 0) !=
      static_cast<ssize_t>(hdr.size())) {
    return false;
  }
  return getU32({hdr.data() + kVersionOffset, 4}) != kJournalFormatVersion;
}

// ── readU64File / writeU64File ─────────────────────────────────────────────
// Raw u64 file I/O for non-cursor values (e.g. continuity.ms).
// Byte-identical to the cursor codec (LE u64), but in a distinct public API
// so the caller's intent is clear: this is NOT a consumer cursor.

auto readU64File(const std::string& path) -> std::int64_t {
  return readCursorImpl(path);
}

auto writeU64File(const std::string& path, std::int64_t v) -> bool {
  return writeCursorImpl(path, v);
}

// ── Journal methods ────────────────────────────────────────────────────────

Journal::Journal(std::string path) : path_{std::move(path)} {}

Journal::Journal(std::string state_root, std::string name)
    : path_{state_root + "/topics/" + name + ".log"},
      state_root_{std::move(state_root)},
      name_{std::move(name)} {}

// Read the UUID from the file header and derive the tag via FNV-1a.
// Returns 0 when the file doesn't exist or is too short (unbound).
// Caches the first non-zero result so subsequent calls skip the I/O.
auto Journal::tag() const -> std::uint64_t {
  if (cached_tag_ != 0) return cached_tag_;
  Fd fd{::open(path_.c_str(), O_RDONLY)};
  if (!fd.valid()) return 0;
  std::array<std::byte, kUuidOffset + kUuidBytes> hdr{};
  if (::pread(fd.get(), hdr.data(), hdr.size(), 0) !=
      static_cast<ssize_t>(hdr.size())) {
    return 0;
  }
  const std::span<const std::byte> uuid{hdr.data() + kUuidOffset, kUuidBytes};
  // An all-zero UUID means the header was never written; treat as unbound.
  bool allzero = true;
  for (const auto b : uuid) {
    if (b != std::byte{0}) { allzero = false; break; }
  }
  if (allzero) return 0;
  const auto t = journalTagFromUuid(uuid);
  cached_tag_ = t;  // only cache non-zero (journalTagFromUuid remaps 0→1)
  return t;
}

auto Journal::append(std::span<const std::byte> payload, Durability durability)
    -> Result<std::string> {
  if (payload.size() > 0xFFFFFFFFULL) {
    return std::unexpected{err("payload too large")};
  }

  std::error_code ec;
  fs::create_directories(fs::path{path_}.parent_path(), ec);
  if (ec) return std::unexpected{err("create topics dir: " + ec.message())};

  const auto created = ensureHeader(path_);
  if (!created) return std::unexpected{created.error()};

  auto append_ms = nowMs();
  const auto seq = nextSeq(append_ms);  // may bump append_ms (clock-rewind safe)

  // v6 record layout:
  //   crc(4) | front_len(8) | append_ms(8) | seq(2) | payload(n) |
  //   trailer_len(8)
  // Total = kRecordOverhead + n = 30+n
  const std::size_t total = kRecordOverhead + payload.size();
  std::vector<std::byte> record;
  record.reserve(total);

  putU32(record, 0);   // crc placeholder; patched after the trailer lands
  putU64(record, 0);   // front record_len placeholder; patched once size known
  putU64(record, static_cast<std::uint64_t>(append_ms));
  putU16(record, seq);
  putBytes(record, payload);
  putU64(record, 0);   // trailer record_len placeholder

  // Patch front record_len (at offset 4) and trailer (at end - 8).
  patchU64(record, 4, record.size());
  patchU64(record, record.size() - 8, record.size());  // trailer == front

  // CRC covers everything after the crc field: [record_len .. trailer].
  const auto crc = Crc32c({record.data() + 4, record.size() - 4});
  patchU32(record, 0, crc);

  if (record.size() > kJournalMaxRecordBytes) {
    return std::unexpected{err(std::format(
        "record size {} exceeds runaway-protection cap {}",
        record.size(), kJournalMaxRecordBytes))};
  }

  Fd fd{::open(path_.c_str(), O_WRONLY | O_APPEND)};
  if (!fd.valid()) {
    return std::unexpected{errFromErrno(errno, "open journal for append")};
  }
  // Torn-write rollback: capture pre-write size; if write fails, truncate
  // back to presize so a torn prefix never poisons the log.
  struct stat st;
  if (::fstat(fd.get(), &st) != 0) {
    return std::unexpected{errFromErrno(errno, "fstat for append")};
  }
  const off_t presize = st.st_size;

  std::size_t written = 0;
  while (written < record.size()) {
    const auto n =
        ::write(fd.get(), record.data() + written, record.size() - written);
    if (n > 0) {
      written += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  if (written != record.size()) {
    const int e = errno;
    if (::ftruncate(fd.get(), presize) != 0) {
      return std::unexpected{
          errFromErrno(errno, "ftruncate rollback failed — log poisoned")};
    }
    return std::unexpected{errFromErrno(e, "append record (rolled back)")};
  }

  if (durability == Durability::Synced) {
    if (::fdatasync(fd.get()) != 0) {
      return std::unexpected{errFromErrno(errno, "fdatasync record")};
    }
    if (*created) {
      const fs::path parent = fs::path{path_}.parent_path();
      Fd dir{::open(parent.c_str(), O_RDONLY)};
      if (!dir.valid()) {
        return std::unexpected{errFromErrno(errno, "open topics dir for fsync")};
      }
      if (::fsync(dir.get()) != 0) {
        return std::unexpected{errFromErrno(errno, "fsync topics dir")};
      }
    }
  }
  return std::format("{:013}-{:04x}", append_ms, seq);
}

auto Journal::peek(Cursor from, std::size_t limit,
                   std::int64_t* truncated_at) const
    -> Result<std::vector<Record>> {
  auto buf = readAll(path_);
  if (!buf) return std::unexpected{buf.error()};
  const auto t = this->tag();
  return ParseFromBuffer(*buf, toOffset(from), limit, t, truncated_at);
}

auto Journal::dump(std::int64_t* truncated_at) const
    -> bus::Result<std::vector<Record>> {
  return peek(Cursor{}, SIZE_MAX, truncated_at);
}

auto Journal::tail(std::size_t n, std::int64_t* truncated_at) const
    -> bus::Result<std::vector<Record>> {
  if (n == 0) {
    if (truncated_at) *truncated_at = -1;
    return std::vector<Record>{};
  }

  auto buf_r = readAll(path_);
  if (!buf_r) return std::unexpected{buf_r.error()};
  const auto& buf = *buf_r;
  if (buf.size() < kJournalHeaderBytes) {
    if (truncated_at) *truncated_at = -1;
    return std::vector<Record>{};
  }

  const auto tag = this->tag();

  // minimum record size = kRecordOverhead (30 bytes, empty payload).
  constexpr std::uint64_t kMinRecordLen = kRecordOverhead;
  constexpr std::size_t kTrailerSize = 8;

  std::vector<Record> out;
  out.reserve(n);

  std::size_t pos = buf.size();  // walk up from EOF
  bool torn_tail_fallback = false;
  std::int64_t corruption_offset = -1;  // set on mid-walk corruption

  while (out.size() < n) {
    // Need at least a trailer (8 bytes) past the header.
    if (pos < kJournalHeaderBytes + kTrailerSize) break;

    // Read trailer record_len at [pos-8, pos).
    const std::uint64_t trailer = getU64({buf.data() + pos - 8, 8});
    if (trailer < kMinRecordLen) {
      // Can't be valid — likely a torn tail. Fall back.
      torn_tail_fallback = true;
      break;
    }

    // Pre-subtraction overflow guard: trailer must fit in the data region.
    if (trailer > pos - kJournalHeaderBytes) {
      if (out.empty()) torn_tail_fallback = true;
      break;
    }
    const std::size_t rec_start = pos - static_cast<std::size_t>(trailer);

    // Read front record_len and crc at rec_start.
    if (rec_start + 12 > buf.size()) {
      if (out.empty()) torn_tail_fallback = true;
      break;
    }
    const std::uint32_t crc = getU32({buf.data() + rec_start, 4});
    const std::uint64_t front_len = getU64({buf.data() + rec_start + 4, 8});

    // front_len must equal trailer.
    if (front_len != trailer) {
      if (out.empty()) torn_tail_fallback = true;
      break;
    }

    // CRC check: covers [record_len .. trailer] = [rec_start+4, pos).
    if (Crc32c({buf.data() + rec_start + 4,
                static_cast<std::size_t>(front_len - 4)}) != crc) {
      if (out.empty()) {
        torn_tail_fallback = true;
        break;
      }
      // Mid-walk corruption: record at rec_start is bad.
      corruption_offset = static_cast<std::int64_t>(rec_start);
      break;
    }

    // Parse the fields.
    std::size_t p = rec_start + 12;  // past crc + front record_len
    const std::uint64_t append_ms = getU64({buf.data() + p, 8});
    p += 8;
    const std::uint16_t seq = getU16({buf.data() + p, 2});
    p += 2;
    const std::uint64_t plen = front_len - kRecordOverhead;
    std::vector<std::byte> payload(buf.data() + p, buf.data() + p + plen);

    out.push_back(Record{
        .id = std::format("{:013}-{:04x}", append_ms, seq),
        .append_ms = static_cast<std::int64_t>(append_ms),
        .payload = std::move(payload),
        .position = Journal::Cursor{static_cast<std::int64_t>(rec_start), tag},
    });
    pos = rec_start;
  }

  if (torn_tail_fallback) {
    // The last record is torn — fall back to a full forward scan + take last n.
    auto all_r = dump(truncated_at);
    if (!all_r) return std::unexpected{all_r.error()};
    auto& all = *all_r;
    if (all.size() <= n) return all;
    return std::vector<Record>(all.end() - static_cast<std::ptrdiff_t>(n),
                               all.end());
  }

  // Reverse to chronological (oldest-of-window first).
  std::reverse(out.begin(), out.end());
  if (truncated_at) *truncated_at = corruption_offset;
  return out;
}

// ── Journal cursor-store methods ──────────────────────────────────────────

auto Journal::cursorFilePath(std::string_view consumer) const -> std::string {
  return cursorPathImpl(state_root_, name_, consumer);
}

auto Journal::lastIdFilePath(std::string_view consumer) const -> std::string {
  return lastIdPathImpl(cursorFilePath(consumer));
}

auto Journal::consumerCursor(std::string_view consumer) const -> Cursor {
  if (state_root_.empty()) {
    fatal("consumerCursor: journal has no state_root+name; "
          "construct with Journal(state_root, name)");
  }
  const auto offset = readCursorImpl(cursorFilePath(consumer));
  return Cursor{offset, tag()};
}

auto Journal::ack(std::string_view consumer, Cursor target,
                  std::string_view id) -> bool {
  if (state_root_.empty()) {
    fatal("ack: journal has no state_root+name; "
          "construct with Journal(state_root, name)");
  }
  // Cross-journal guard: a non-zero tag on target that differs from this
  // journal's tag is always a programmer error — it means the caller is
  // acking the wrong journal. Crash immediately; silent corruption would
  // be far harder to diagnose.
  // tag() == 0 means the file doesn't exist yet (unbound); skip the check
  // so an un-created journal never crashes on a tagged cursor.
  const auto my_tag = tag();
  if (my_tag != 0 && target.journal_tag_ != 0 &&
      target.journal_tag_ != my_tag) {
    fatal("ack: cross-journal cursor — cursor was issued by a different "
          "journal; check that cursor origin matches the journal being acked");
  }
  const auto path = cursorFilePath(consumer);
  if (!advanceCursorMonotonicImpl(path, target.offset_)) return false;
  if (!id.empty()) writeLastIdImpl(lastIdPathImpl(path), std::string{id});
  return true;
}

auto Journal::lastAckedId(std::string_view consumer) const -> std::string {
  if (state_root_.empty()) {
    fatal("lastAckedId: journal has no state_root+name; "
          "construct with Journal(state_root, name)");
  }
  return readLastIdImpl(lastIdFilePath(consumer));
}

// ── Opaque persistence tokens (forwarded from Cursor) ─────────────────────

auto Journal::cursorToToken(Cursor c) -> std::string {
  return c.toToken();
}

auto Journal::cursorFromToken(std::string_view token) -> Cursor {
  return Cursor::fromToken(token);
}

// ── Journal::cursorAfter ──────────────────────────────────────────────────

auto Journal::cursorAfter(const Record& r) -> Cursor {
  // Derived from position + kRecordOverhead + payload size. The tag is
  // inherited from position so the resulting cursor stays bound to the same
  // named journal (tag 0 for unbound/unnamed journals).
  const std::int64_t next =
      r.position.offset_ + static_cast<std::int64_t>(kRecordOverhead) +
      static_cast<std::int64_t>(r.payload.size());
  return Cursor{next, r.position.journal_tag_};
}

// ── Journal::toOffset ──────────────────────────────────────────────────────

auto Journal::toOffset(Cursor c) -> std::int64_t {
  // Private — callers hold Cursors opaquely; only the kernel needs the raw
  // byte position to drive the read path (peek, consumerCursor, cursorAfter).
  return c.offset_;
}

}  // namespace bus
