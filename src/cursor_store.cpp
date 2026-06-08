#include "cursor_store.h"

#include "journal.h"
#include "state_paths.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace bus {


namespace {

// ── Cursor-path helpers ────────────────────────────────────────────────────

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
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return 0;
  std::array<std::byte, 8> buf{};
  const auto n = ::read(fd, buf.data(), buf.size());
  ::close(fd);
  if (n != 8) return 0;
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buf[i]))
         << (i * 8);
  }
  return static_cast<std::int64_t>(v);
}

auto writeCursorImpl(const std::string& path, std::int64_t offset) -> bool {
  std::error_code ec;
  fs::create_directories(fs::path{path}.parent_path(), ec);
  if (ec) return false;
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  std::array<std::byte, 8> buf{};
  const auto v = static_cast<std::uint64_t>(offset);
  for (int i = 0; i < 8; ++i) buf[i] = std::byte((v >> (i * 8)) & 0xFF);
  if (::write(fd, buf.data(), buf.size()) != 8) {
    ::close(fd);
    return false;
  }
  ::close(fd);
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
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return {};
  std::array<char, 256> buf{};
  const auto n = ::read(fd, buf.data(), buf.size());
  ::close(fd);
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
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const std::string line = id + "\n";
  if (::write(fd, line.data(), line.size()) !=
      static_cast<ssize_t>(line.size())) {
    ::close(fd);
    return false;
  }
  ::close(fd);
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

// ── CursorStore ───────────────────────────────────────────────────────────

CursorStore::CursorStore(std::string state_root, std::string name)
    : state_root_{std::move(state_root)}, name_{std::move(name)} {}

auto CursorStore::cursorFilePath(std::string_view consumer) const -> std::string {
  return cursorPathImpl(state_root_, name_, consumer);
}

auto CursorStore::lastIdFilePath(std::string_view consumer) const -> std::string {
  return lastIdPathImpl(cursorFilePath(consumer));
}

auto CursorStore::consumerCursor(std::string_view consumer) const
    -> Journal::Cursor {
  const std::string journal_path = topicLogPath(state_root_, name_);
  const auto offset = readCursorImpl(cursorFilePath(consumer));
  const auto tag = Journal::tagOf(journal_path);
  return Journal::Cursor{offset, tag};
}

auto CursorStore::ack(std::string_view consumer, Journal::Cursor target,
                      std::string_view id) -> bool {
  const std::string journal_path = topicLogPath(state_root_, name_);
  // Cross-journal guard: a non-zero tag on target that differs from this
  // journal's tag is always a programmer error.
  const auto my_tag = Journal::tagOf(journal_path);
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

auto CursorStore::lastAckedId(std::string_view consumer) const -> std::string {
  return readLastIdImpl(lastIdFilePath(consumer));
}

}  // namespace bus
