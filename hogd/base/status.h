// status.h - error handling

#pragma once

#include <cerrno>
#include <concepts>
#include <expected>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "x23/err.h"

namespace hogd {

using x23::Errno;
using x23::ErrorBase;
using x23::make_error_code;
using x23::make_error_condition;

// ErrnoStatus is the return type for a function that can return a legacy errno.
//
// Example:
// ErrnoStatus<void> Foo()
// {
//   if (fstat(fd, &sb) == -1)
//     return Errno();
// }
//
// ErrnoStatus<void> ret = Foo();
// if (!foo) // handle error
template<typename T> using ErrnoStatus = std::expected<T, Errno>;

// make_errno_status converts a system call return value into an ErrnoStatus
// of a given result type. This is useful because many system calls return -1
// on failure and set errno, or else the return value is the result (e.g.,
// open() returns an fd number on success).
template<typename Result, typename T>
ErrnoStatus<Result>
make_errno_status(T ret)
{
  // first check for error.
  if (ret == -1)
    return Errno{};

  // otherwise return the result type.
  assert(ret >= 0);
  if constexpr (std::is_integral_v<Result>) {
    return static_cast<Result>(ret);
  }
  else if (std::is_void_v<Result>) {
    assert(ret == 0);
    return {};
  }
  else {
    static_assert("unsupported result type");
  }
}

// EnumError turns a scoped enumeration into an error class.
template<typename Enum, std::derived_from<std::error_category> Category>
requires std::is_scoped_enum_v<Enum>
class EnumError : public ErrorBase {
public:
  explicit EnumError(Enum code) : code_(code) {}

  // code returns the enum error code.
  Enum code() const { return code_; }
  // raw_code returns the error code as an int.
  int raw_code() const { return static_cast<int>(std::to_underlying(code_)); }

  // is_in_enum_range returns true if a raw code lands within the enumeration.
  //
  // This is a good thing to check or assert since it is undefined behavior to
  // cast a value to an enum class if it is outside the range.
  constexpr static bool is_in_enum_range(int raw_code)
  {
    using underlying_enum = std::underlying_type_t<Enum>;
    return (raw_code >= std::numeric_limits<underlying_enum>::min() &&
            raw_code <= std::numeric_limits<underlying_enum>::max());
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
// Enable comparisons between EnumError and std::error_code.
template<typename E, typename Category> requires std::is_scoped_enum_v<E>
inline bool
operator==(const EnumError<E, Category> &lhs, const std::error_code &rhs)
{
  return lhs.category() == rhs.category() && lhs.raw_code() == rhs.value();
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
  const char *name() const noexcept override { return "grpc"; }
  std::string message(int code) const override;
};

// GError is the error object for GRPC errors.
using GError = EnumError<GCode, GCategory>;

// Enable comparisons between GCode and std::error_code.
inline bool
operator==(const GCode &lhs, const std::error_code &rhs)
{
  return GError{lhs} == rhs;
}

// GStatus is the return type for a function that can return a GRPC error.
//
// Example:
// GStatus<void> Foo()
// {
//   return GError(GCode::kUnimplemented);
// }
//
// GStatus<void> ret = Foo();
// if (!foo) // handle error
template<typename T> using GStatus = std::expected<T, GError>;

} // namespace hogd
