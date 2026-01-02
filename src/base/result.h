#pragma once

#include <cassert>
#include <optional>
#include <utility>

#include "base/error.h"

namespace base {

// Tag returned by fail(); implicitly converts to any Result<T>.
struct Failure {
  ErrorCode code;
};

constexpr Failure fail(ErrorCode code) noexcept { return Failure{code}; }

// Hot paths return Result<T> rather than throwing (ER-3). Exceptions are permitted
// only during construction/config, where a failure means the process should not start.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Failure f) : error_(f.code) {           // NOLINT(google-explicit-constructor)
    assert(f.code != ErrorCode::kNone && "fail(kNone) is a contradiction");
  }

  [[nodiscard]] bool ok() const noexcept { return error_ == ErrorCode::kNone; }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] ErrorCode error() const noexcept { return error_; }

  T& value() & {
    assert(ok());
    return *value_;
  }
  const T& value() const& {
    assert(ok());
    return *value_;
  }
  T&& value() && {
    assert(ok());
    return std::move(*value_);
  }

  T value_or(T fallback) const& { return ok() ? *value_ : std::move(fallback); }

 private:
  std::optional<T> value_;
  ErrorCode error_ = ErrorCode::kNone;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Failure f) : error_(f.code) {  // NOLINT(google-explicit-constructor)
    assert(f.code != ErrorCode::kNone && "fail(kNone) is a contradiction");
  }

  [[nodiscard]] bool ok() const noexcept { return error_ == ErrorCode::kNone; }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] ErrorCode error() const noexcept { return error_; }

 private:
  ErrorCode error_ = ErrorCode::kNone;
};

using Status = Result<void>;

}  // namespace base
