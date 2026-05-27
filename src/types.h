#pragma once

#include <expected>
#include <string>

namespace bus {

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

  Error() = default;
  Error(std::string msg) : code(ErrorCode::Unknown), message(std::move(msg)) {}
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

template <typename T>
using Result = std::expected<T, Error>;

}  // namespace bus
