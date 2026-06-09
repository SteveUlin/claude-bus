#include "cursor_store.h"

#include "journal.h"
#include "state_paths.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <filesystem>
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

// ── Persisted cursor: offset + journal tag ────────────────────────────────

struct PersistedCursor {
  std::int64_t offset{0};
  std::uint64_t tag{0};  // 0 = legacy file or unbound
};

// On-disk format (new): "<offset>:<tag>\n" — same "<offset>:<tag>" codec
// as Cursor::toToken(), written as a text line.
//
// Legacy format (old fleet files): exactly 8 bytes, raw little-endian u64
// offset. An 8-byte read is treated as legacy unless every byte is in the
// text format's character class (digits, ':', '\n') — a raw binary offset
// can coincidentally contain 0x3A (':'), so presence of a colon alone is
// not a safe discriminator, but binary offsets are never all-ASCII-digits.
// On legacy files the tag is adopted as 0 (today's behavior is preserved;
// the new tagged format is written on the next ack).
//
// Corrupt / unreadable files: return {0, 0} — same as "no file" and
// "start-of-log", matching the existing behavior.
auto readCursorFileImpl(const std::string& path) -> PersistedCursor {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return {};
  // Read enough for the new text format (offsets up to ~20 digits, tag up to
  // 20 digits, colon, newline = at most ~43 bytes; 64 is safe).
  std::array<char, 64> buf{};
  const auto n = ::read(fd, buf.data(), buf.size());
  ::close(fd);
  if (n <= 0) return {};

  // Legacy detection: exactly 8 bytes, not all in the text character class
  // → raw LE u64 offset.
  if (n == 8) {
    bool text_like = true;
    for (int i = 0; i < 8; ++i) {
      const char c = buf[i];
      if (!((c >= '0' && c <= '9') || c == ':' || c == '\n')) {
        text_like = false;
        break;
      }
    }
    if (!text_like) {
      std::uint64_t v = 0;
      for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(
                 static_cast<std::uint8_t>(buf[i])) << (i * 8);
      }
      return {static_cast<std::int64_t>(v), 0};
    }
  }

  // New text format: "<offset>:<tag>\n"
  const std::string_view sv{buf.data(), static_cast<std::size_t>(n)};
  try {
    const auto colon = sv.find(':');
    if (colon == std::string_view::npos) return {};
    const auto offset = static_cast<std::int64_t>(
        std::stoll(std::string{sv.substr(0, colon)}));
    const auto tag_sv = sv.substr(colon + 1);
    const auto tag = static_cast<std::uint64_t>(
        std::stoull(std::string{tag_sv}));
    return {offset, tag};
  } catch (...) {
    return {};
  }
}

auto writeCursorFileImpl(const std::string& path,
                         std::int64_t offset,
                         std::uint64_t tag) -> bool {
  std::error_code ec;
  fs::create_directories(fs::path{path}.parent_path(), ec);
  if (ec) return false;
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const std::string text =
      std::to_string(offset) + ":" + std::to_string(tag) + "\n";
  if (::write(fd, text.data(), text.size()) !=
      static_cast<ssize_t>(text.size())) {
    ::close(fd);
    return false;
  }
  ::close(fd);
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

auto advanceCursorMonotonicImpl(const std::string& path,
                                std::int64_t target,
                                std::uint64_t tag) -> bool {
  if (readCursorFileImpl(path).offset >= target) return false;
  return writeCursorFileImpl(path, target, tag);
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
  const auto persisted = readCursorFileImpl(cursorFilePath(consumer));
  const auto journal_tag = Journal::tagOf(journal_path);
  // Cross-journal stale-file guard: if the cursor file carries a non-zero tag
  // that doesn't match the current journal's tag, the log was deleted and
  // recreated under the same name. The stale offset is unsafe to apply —
  // silently reset to start-of-log so the consumer replays from the beginning
  // of the new journal. The next ack will persist the new tag.
  if (persisted.tag != 0 && journal_tag != 0 &&
      persisted.tag != journal_tag) {
    return Journal::Cursor{0, journal_tag};
  }
  return Journal::Cursor{persisted.offset, journal_tag};
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
  if (!advanceCursorMonotonicImpl(path, target.offset_, my_tag)) return false;
  if (!id.empty()) writeLastIdImpl(lastIdPathImpl(path), std::string{id});
  return true;
}

auto CursorStore::lastAckedId(std::string_view consumer) const -> std::string {
  return readLastIdImpl(lastIdFilePath(consumer));
}

}  // namespace bus
