#pragma once

#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>
#include <unistd.h>

namespace bus {

// Crash with a message. Use for programmer errors (precondition violations,
// invariant breaks). Always active — not stripped under NDEBUG.
[[noreturn]] inline auto fatal(std::string_view msg) -> void {
  std::string s = "bus fatal: ";
  s += msg;
  s += '\n';
  [[maybe_unused]] auto _ = ::write(STDERR_FILENO, s.data(), s.size());
  std::abort();
}

enum class ErrorCode {
  NotFound,
  ParseError,
  IOError,
  InvalidArgument,
  Unknown
};

struct Error {
  ErrorCode code{ErrorCode::Unknown};
  std::string message;

  Error() = delete;
  Error(std::string msg) : code(ErrorCode::Unknown), message(std::move(msg)) {}
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

template <typename T>
using Result = std::expected<T, Error>;

}  // namespace bus
