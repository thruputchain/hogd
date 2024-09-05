// status.h - error handling

#pragma once

#include <expected>
#include <format>
#include <system_error>
#include <type_traits>
#include <utility>

#include <errno.h>

#include "hogd/base/types.h"

namespace hogd {

// ErrorBase provides a wrapper for all error types. It provides
// pretty-printing through std::format, automatic conversion to unexpected when
// returning from functions with std::expected, and compatibility with the std
// library's error handling. It can also be thrown as a std::system_error via
// raise().
//
// ErrorBase uses the CRTP idiom, so it doesn't add any vtable overhead.
//
// The error class must inherit ErrorBase and provide the following methods:
// 1. int raw_code()
// 2. std::error_category category() // usually static
//
// In addition, it should include its own operator== definition.
class ErrorBase {
public:
  // provides automatic conversion to std::unexpected
  template<typename Self, typename T, typename E>
  requires std::convertible_to<Self, E>
  [[nodiscard]] operator std::expected<T, E>(this const Self & self) noexcept
  {
    return std::unexpected(self);
  }

  // error_code converts the derived error into a std::error_code.
  [[nodiscard]] std::error_code error_code(this const auto &self)
  {
    return std::error_code(self.raw_code(), self.category());
  }

  // raise converts the derived error to a system error and throws it.
  [[noreturn]] void raise(this const auto &self)
  {
    throw std::system_error(self.error_code());
  }

  // raise converts the derived error to a system error that includes additional
  // text and throws it.
  template<typename... Args>
  [[noreturn]] void raise(this const auto &self,
                          std::format_string<Args...> fmt, Args &&...args)
  {
    throw std::system_error{self.code(),
                            std::vformat(fmt.get(),
                                         std::make_format_args(args...))};
  }

protected:
  ErrorBase() noexcept = default;
};

// make_error_code converts an ErrorBase derived class into a std::error_code.
[[nodiscard]] inline std::error_code
make_error_code(const std::derived_from<ErrorBase> auto &e)
{
  return e.error_code();
}

// make_error_condition converts an ErrorBase derived class into a
// std::error_condition.
[[nodiscard]] inline std::error_condition
make_error_condition(const std::derived_from<ErrorBase> auto &e)
{
  return make_error_code(e).default_error_condition();
}

// Errno provides a wrapper around errno.
//
// Example:
// if (fstat(fd, &sb) == -1) {
//   Errno e;
//   std::println(stderr, "cannot stat file: {}", e);
//   return e;   // converts to std::unexpected
// }
class Errno : public ErrorBase {
public:
  Errno() noexcept : err_(errno) {}
  explicit Errno(int err) noexcept : err_(err) {}

  [[nodiscard]] int raw_code() const { return err_; }
  static const std::error_category &category()
  {
    return std::system_category();
  }

private:
  int err_;
};

// Enable comparisons between Errno objects.
inline bool
operator==(const Errno &lhs, const Errno &rhs)
{
  return lhs.raw_code() == rhs.raw_code();
}
// Enable comparisons between Errno and legacy errno codes.
inline bool
operator==(const Errno &lhs, const int &rhs)
{
  return lhs.raw_code() == rhs;
}

// EnumError turns a scoped enumeration into an error class.
template<typename Enum, std::derived_from<std::error_category> Category>
requires std::is_scoped_enum_v<Enum>
class EnumError : public ErrorBase {
public:
  explicit EnumError(Enum code) : code_(code) {}

  // code returns the enum error code.
  [[nodiscard]] Enum code() const { return code_; }
  // raw_code returns the error code as an int.
  [[nodiscard]] int raw_code() const
  {
    return static_cast<int>(std::to_underlying(code_));
  }
  // category returns the std::error_category for this error type.
  static const std::error_category &category()
  {
    static const Category cat;
    return cat;
  }

private:
  Enum code_;
};

// Enable comparisons between EnumError objects.
template<typename E, typename Category>
inline bool
operator==(const EnumError<E, Category> &lhs, const EnumError<E, Category> &rhs)
{
  return lhs.code() == rhs.code();
}
// Enable comparisons between EnumError objects and scoped enums directly.
template<typename E, typename Category> requires std::is_scoped_enum_v<E>
inline bool
operator==(const EnumError<E, Category> &lhs, const E &rhs)
{
  return lhs.code() == rhs;
}

// GCode is an enum of the GRPC error codes. They are especially useful for
// building distributed systems. The integer numbers used below are also the
// same as those defined in GRPC.
//
// More documentation here: https://grpc.io/docs/guides/status-codes/
enum class GCode : uint8_t {
  // Operation was cancelled (typically by the caller)
  kCancelled = 1,

  // Unknown error
  kUnknown = 2,

  // Invalid argument from caller
  kInvalidArgument = 3,

  // Deadline exceeded
  kDeadlineExceeded = 4,

  // A requested entity (e.g., file or directory) was not found
  kNotFound = 5,

  // The entity that the caller attempted to create exists
  kAlreadyExists = 6,

  // Permission denied
  kPermissionDenied = 7,

  // Resource exhausted (e.g., quota or disk out of space)
  kResourceExhausted = 8,

  // Failed precondition (e.g., the directory to be deleted was not empty)
  kFailedPrecondition = 9,

  // The operation was aborted (typically a failed transaction)
  kAborted = 10,

  // The operation was attempted past the valid range
  kOutOfRange = 11,

  // The operation is not implemented or the feature is not supported
  kUnimplemented = 12,

  // Internal error. An invariant in the underlying system was broken
  kInternal = 13,

  // Resource is unavailable. Try again (generally after exponential backoff)
  kUnavailable = 14,

  // Unrecoverable data loss or corruption
  kDataLoss = 15,

  // Caller does not have valid authentication tokens to complete the operation
  kUnauthorized = 16,
};

// GCategory provides an error category for GRPC errors.
class GCategory : public std::error_category {
public:
  [[nodiscard]] const char *name() const noexcept override { return "grpc"; }
  [[nodiscard]] std::string message(int code) const override
  {
    switch (static_cast<GCode>(code)) {
    case GCode::kCancelled:
      return "CANCELLED";
    case GCode::kUnknown:
      return "UNKNOWN";
    case GCode::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case GCode::kDeadlineExceeded:
      return "DEADLINE_EXCEEDED";
    case GCode::kNotFound:
      return "NOT_FOUND";
    case GCode::kAlreadyExists:
      return "ALREADY_EXISTS";
    case GCode::kPermissionDenied:
      return "PERMISSION_DENIED";
    case GCode::kResourceExhausted:
      return "RESOURCE_EXHAUSTED";
    case GCode::kFailedPrecondition:
      return "FAILED_PRECONDITION";
    case GCode::kAborted:
      return "ABORTED";
    case GCode::kOutOfRange:
      return "OUT_OF_RANGE";
    case GCode::kUnimplemented:
      return "UNIMPLEMENTED";
    case GCode::kInternal:
      return "INTERNAL";
    case GCode::kUnavailable:
      return "UNAVAILABLE";
    case GCode::kDataLoss:
      return "DATA_LOSS";
    case GCode::kUnauthorized:
      return "UNAUTHORIZED";
    default:
      return "";
    }
  }
};

// GError is the error object for GRPC errors.
using GError = EnumError<GCode, GCategory>;

// GStatus is the return type for a function that can return a GRPC error.
//
// Example:
// GStatus<void> Foo()
// {
//   return GError(GCode::kUnimplemented);
// }
//
// Gstatus<void> ret = Foo();
// if (!foo) // handle error
template<typename T> using GStatus = std::expected<T, GError>;

} // namespace hogd

// make everything that inherits ErrorBase an error_code_enum
template<std::derived_from<hogd::ErrorBase> E>
struct std::is_error_code_enum<E> : std::true_type {};

// provide format support for everything that inherits ErrorBase
template<std::derived_from<hogd::ErrorBase> E>
class std::formatter<E> : public std::formatter<std::string> {
public:
  using super = std::formatter<std::string>;
  auto format(const E &e, auto &ctx) const
  {
    return super::format(make_error_code(e).message(), ctx);
  }
};
