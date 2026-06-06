#include "topic_log.h"

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

namespace bus::topic {

namespace {

constexpr std::size_t kVersionOffset = 4;

// Test seam: a hook over the raw write syscall so tests can fault-inject a
// short / failed write and exercise append()'s torn-record rollback. nullptr
// (the default) routes to ::write. Set via setWriteHookForTesting. The broker
// never sets it; production is byte-identical to a bare ::write.
ssize_t (*g_writeHook)(int, const void*, std::size_t) = nullptr;
auto rawWrite(int fd, const void* buf, std::size_t n) -> ssize_t {
  return g_writeHook != nullptr ? g_writeHook(fd, buf, n) : ::write(fd, buf, n);
}

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

auto randU16() -> std::uint16_t {
  thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<unsigned> dist{0, 0xFFFF};
  return static_cast<std::uint16_t>(dist(rng));
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
auto putBytes(std::vector<std::byte>& b, std::string_view s) -> void {
  for (char c : s) b.push_back(std::byte(c));
}
auto putBytes(std::vector<std::byte>& b,
              std::span<const std::uint8_t> s) -> void {
  for (auto v : s) b.push_back(std::byte(v));
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

auto makeFileHeader() -> std::array<std::byte, kFileHeaderBytes> {
  std::array<std::byte, kFileHeaderBytes> h{};
  h[0] = std::byte{'B'};
  h[1] = std::byte{'U'};
  h[2] = std::byte{'S'};
  h[3] = std::byte{0};
  const std::uint32_t v = kFormatVersion;
  for (int i = 0; i < 4; ++i) {
    h[kVersionOffset + i] = std::byte((v >> (i * 8)) & 0xFF);
  }
  return h;
}

auto ensureHeader(const std::string& path) -> Result<void> {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) {
    Fd guard{fd};
    const auto h = makeFileHeader();
    if (::pwrite(fd, h.data(), kFileHeaderBytes, 0) !=
        static_cast<ssize_t>(kFileHeaderBytes)) {
      return std::unexpected{errFromErrno(errno, "write header")};
    }
    return {};
  }
  if (errno != EEXIST) {
    return std::unexpected{errFromErrno(errno, "init topic log")};
  }
  return {};
}

auto readAll(const std::string& path)
    -> Result<std::vector<std::byte>> {
  Fd fd{::open(path.c_str(), O_RDONLY)};
  if (!fd.valid()) {
    if (errno == ENOENT) return std::vector<std::byte>{};
    return std::unexpected{errFromErrno(errno, "open topic log")};
  }
  struct stat st;
  if (::fstat(fd.get(), &st) != 0) {
    return std::unexpected{errFromErrno(errno, "fstat")};
  }
  std::vector<std::byte> buf(static_cast<std::size_t>(st.st_size));
  if (buf.empty()) return buf;
  if (::pread(fd.get(), buf.data(), buf.size(), 0) !=
      static_cast<ssize_t>(buf.size())) {
    return std::unexpected{errFromErrno(errno, "pread topic log")};
  }
  if (buf.size() >= kFileHeaderBytes) {
    const auto v = getU32({buf.data() + kVersionOffset, 4});
    if (v != kFormatVersion) {
      return std::unexpected{err(std::format(
          "topic log {} has format v{}, runtime expects v{}; wipe and retry",
          path, v, kFormatVersion))};
    }
  }
  return buf;
}

}  // namespace

// Exposed (declared in topic_log.h) so the binary wire parser can be
// table-tested directly with hand-built buffers — including truncated
// tails — without going through readAll + a real file. Calls the
// anonymous-namespace getU* helpers above (same translation unit).
auto parseFrom(std::span<const std::byte> buf, std::int64_t start_offset,
               std::size_t limit) -> std::vector<Message> {
  std::vector<Message> out;
  if (buf.size() < kFileHeaderBytes) return out;
  std::size_t pos = start_offset > static_cast<std::int64_t>(kFileHeaderBytes)
                        ? static_cast<std::size_t>(start_offset)
                        : kFileHeaderBytes;
  while (pos + 8 <= buf.size() && out.size() < limit) {
    const std::uint64_t rec_len = getU64({buf.data() + pos, 8});
    if (rec_len < 8 || pos + rec_len > buf.size()) break;

    std::size_t p = pos + 8;
    if (p + 8 > buf.size()) break;
    const std::uint64_t sent_ms = getU64({buf.data() + p, 8});
    p += 8;
    if (p + 2 > buf.size()) break;
    const std::uint16_t rand_tag = getU16({buf.data() + p, 2});
    p += 2;
    if (p + 4 > buf.size()) break;
    const std::uint32_t ttl_ms = getU32({buf.data() + p, 4});
    p += 4;
    if (p + 1 > buf.size()) break;
    const auto deliver_when = std::to_integer<std::uint8_t>(buf[p]);
    p += 1;
    if (p + 1 > buf.size()) break;
    const auto slen = std::to_integer<std::uint8_t>(buf[p]);
    p += 1;
    if (p + slen > buf.size()) break;
    std::string sender(reinterpret_cast<const char*>(buf.data() + p), slen);
    p += slen;
    if (p + 1 > buf.size()) break;
    const auto plen = std::to_integer<std::uint8_t>(buf[p]);
    p += 1;
    if (p + plen > buf.size()) break;
    std::string protocol(reinterpret_cast<const char*>(buf.data() + p), plen);
    p += plen;
    if (p + 16 > buf.size()) break;
    std::array<std::uint8_t, 16> correlation{};
    for (int i = 0; i < 16; ++i) {
      correlation[i] = std::to_integer<std::uint8_t>(buf[p + i]);
    }
    p += 16;
    if (p + 4 > buf.size()) break;
    const std::uint32_t blen = getU32({buf.data() + p, 4});
    p += 4;
    if (p + blen > buf.size()) break;
    std::string body(reinterpret_cast<const char*>(buf.data() + p), blen);

    out.push_back(Message{
        .id = std::format("{:013}-{}-{:04x}", sent_ms, sender, rand_tag),
        .sent_ms = static_cast<std::int64_t>(sent_ms),
        .sender = std::move(sender),
        .ttl_ms = ttl_ms,
        .deliver_when = deliver_when,
        .protocol = std::move(protocol),
        .correlation = correlation,
        .body = std::move(body),
        .offset = static_cast<std::int64_t>(pos),
        .next_offset = static_cast<std::int64_t>(pos + rec_len),
    });
    pos += rec_len;
  }
  return out;
}

TopicLog::TopicLog(std::string path) : path_{std::move(path)} {}

auto TopicLog::append(std::string_view sender, std::string_view body,
                      const SendOpts& opts) -> Result<std::string> {
  if (opts.protocol.size() > 0xFF) {
    return std::unexpected{err("protocol too long (>255 bytes)")};
  }
  if (sender.size() > 0xFF) {
    return std::unexpected{err("sender too long (>255 bytes)")};
  }
  if (body.size() > 0xFFFFFFFFULL) {
    return std::unexpected{err("body too large")};
  }

  std::error_code ec;
  fs::create_directories(fs::path{path_}.parent_path(), ec);
  if (ec) return std::unexpected{err("create topics dir: " + ec.message())};

  if (auto r = ensureHeader(path_); !r) return std::unexpected{r.error()};

  const auto sent_ms = nowMs();
  const auto rand_tag = randU16();

  std::vector<std::byte> record;
  record.reserve(48 + sender.size() + opts.protocol.size() + body.size());

  putU64(record, 0);  // placeholder; patched after we know size
  putU64(record, static_cast<std::uint64_t>(sent_ms));
  putU16(record, rand_tag);
  putU32(record, opts.ttl_ms);
  record.push_back(std::byte(opts.deliver_when));
  record.push_back(std::byte(static_cast<std::uint8_t>(sender.size())));
  putBytes(record, sender);
  record.push_back(std::byte(static_cast<std::uint8_t>(opts.protocol.size())));
  putBytes(record, opts.protocol);
  putBytes(record, std::span<const std::uint8_t>{opts.correlation.data(),
                                                 opts.correlation.size()});
  putU32(record, static_cast<std::uint32_t>(body.size()));
  putBytes(record, body);

  patchU64(record, 0, record.size());

  if (record.size() > kMaxRecordBytes) {
    return std::unexpected{err(std::format(
        "record size {} exceeds runaway-protection cap {}",
        record.size(), kMaxRecordBytes))};
  }

  Fd fd{::open(path_.c_str(), O_WRONLY | O_APPEND)};
  if (!fd.valid()) {
    return std::unexpected{errFromErrno(errno, "open topic log for append")};
  }
  // All-or-nothing append (kernel-0-0). A short ::write under O_APPEND leaves a
  // TORN record mid-log: its length prefix claims the full size but fewer bytes
  // follow, so a subsequent append lands right after the partial bytes and
  // parseFrom reads the claimed length into the next record — misframing the
  // entire tail and collapsing cursor addressing + at-least-once (rule #1).
  // Fix: (1) loop the write — retry EINTR, advance on partials; under O_APPEND
  // each write atomically targets EOF, so the record completes contiguously
  // across calls. (2) on a hard error with bytes already written, ftruncate
  // back to the pre-write size so the log is left exactly as it was.
  // SAFE because topic logs are SINGLE-WRITER: every TopicLog::append call site
  // is broker-only (delivery.cpp / broker.cpp; CLI tools go through the enqueue
  // RPC) and the broker is a single-threaded pselect loop, so no concurrent
  // appender can slip a record in between the fstat and the ftruncate. If a
  // second writer is ever introduced this must become write-tmp+rename or an
  // flock around the append.
  struct stat st_before{};
  if (::fstat(fd.get(), &st_before) != 0) {
    return std::unexpected{errFromErrno(errno, "fstat topic log for append")};
  }
  const auto size_before = static_cast<off_t>(st_before.st_size);

  const auto* p = record.data();
  std::size_t remaining = record.size();
  while (remaining > 0) {
    const auto n = rawWrite(fd.get(), p, remaining);
    if (n < 0 && errno == EINTR) continue;  // interrupted — retry
    if (n <= 0) break;                       // hard error / no progress — bail
    p += n;
    remaining -= static_cast<std::size_t>(n);
  }
  if (remaining > 0) {
    const int saved = errno;
    // Roll back the partial bytes so no torn record survives. If the rollback
    // ITSELF fails (rare double-fault), surface THAT — never silently leave a
    // torn log behind.
    if (::ftruncate(fd.get(), size_before) != 0) {
      return std::unexpected{errFromErrno(
          errno, "append record: short write AND ftruncate rollback failed")};
    }
    return std::unexpected{
        errFromErrno(saved, "append record (partial write rolled back)")};
  }
  return std::format("{:013}-{}-{:04x}", sent_ms, sender, rand_tag);
}

auto TopicLog::peek(std::int64_t start_offset, std::size_t limit) const
    -> Result<std::vector<Message>> {
  auto buf = readAll(path_);
  if (!buf) return std::unexpected{buf.error()};
  return parseFrom(*buf, start_offset, limit);
}

auto TopicLog::dump() const -> Result<std::vector<Message>> {
  return peek(static_cast<std::int64_t>(kFileHeaderBytes));
}

auto TopicLog::trimHead(std::int64_t cut_offset) -> std::int64_t {
  const auto header = static_cast<std::int64_t>(kFileHeaderBytes);
  auto buf = readAll(path_);
  if (!buf) return 0;
  const auto size = static_cast<std::int64_t>(buf->size());
  // Out of range / no-op: a cut at or before the header drops nothing; a cut
  // at or past EOF would drop the whole body (refused — that's not a trim).
  if (cut_offset <= header || cut_offset >= size) return 0;

  // Splice [0, header) + [cut_offset, size) into a tmp file, atomic-rename.
  const std::string tmp = path_ + ".trim.tmp";
  Fd fd{::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
  if (!fd.valid()) return 0;
  const auto* base = reinterpret_cast<const char*>(buf->data());
  if (::write(fd.get(), base, static_cast<std::size_t>(header)) !=
      static_cast<ssize_t>(header)) {
    return 0;
  }
  const auto tail = static_cast<std::size_t>(size - cut_offset);
  if (::write(fd.get(), base + cut_offset, tail) !=
      static_cast<ssize_t>(tail)) {
    return 0;
  }
  fd.reset();
  if (::rename(tmp.c_str(), path_.c_str()) != 0) return 0;
  return cut_offset - header;  // bytes dropped == the offset shift
}

auto cursorPath(std::string_view state_root, std::string_view topic,
                std::string_view consumer) -> std::string {
  std::string c{consumer};
  if (c.empty()) c = "_default";
  return std::string{state_root} + "/cursors/" + std::string{topic} + "/" +
         c + ".cursor";
}

auto readCursor(const std::string& path) -> std::int64_t {
  Fd fd{::open(path.c_str(), O_RDONLY)};
  if (!fd.valid()) return 0;
  std::array<std::byte, 8> buf{};
  const auto n = ::read(fd.get(), buf.data(), buf.size());
  if (n != 8) return 0;
  return static_cast<std::int64_t>(getU64({buf.data(), 8}));
}

auto writeCursor(const std::string& path, std::int64_t offset) -> bool {
  std::error_code ec;
  fs::create_directories(fs::path{path}.parent_path(), ec);
  if (ec) return false;
  // Atomic via tmp+rename.
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

auto advanceCursorMonotonic(const std::string& path, std::int64_t target)
    -> bool {
  if (readCursor(path) >= target) return false;  // never rewind
  return writeCursor(path, target);
}

auto lastIdPath(const std::string& cursor_path) -> std::string {
  constexpr std::string_view kSuffix = ".cursor";
  if (cursor_path.size() >= kSuffix.size() &&
      cursor_path.compare(cursor_path.size() - kSuffix.size(),
                          kSuffix.size(), kSuffix) == 0) {
    return cursor_path.substr(0, cursor_path.size() - kSuffix.size()) +
           ".lastid";
  }
  return cursor_path + ".lastid";
}

auto readLastId(const std::string& path) -> std::string {
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

auto writeLastId(const std::string& path, const std::string& id) -> bool {
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

void setWriteHookForTesting(ssize_t (*fn)(int, const void*, std::size_t)) {
  g_writeHook = fn;
}

}  // namespace bus::topic
