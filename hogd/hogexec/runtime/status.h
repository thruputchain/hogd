#pragma once

#include <expected>

#include "hogd/base/status.h"

namespace hogd {

enum class InvokeErrorCode : uint8_t {
  kNone = 0,
  kDeterministicError = 1,
  kOutOfGasError = 2,
  kUnrecoverableSystemError = 3,
};

// Host fn calls have two types of errors:
// 1) Recoverable (possibly handled by the caller contract,
// must generate deterministically -- e.g. accessing a nonexistent key)
// 2) Everything else (Unrecoverable)
// We must be very deliberate about what errors we mark in the first category.
enum class HostFnErrorCode : uint8_t {
  kNoneOrRecoverable = 0,
  kOutOfGas = 1,
  kUnrecoverableUnknown = 2,
  kReturn = 3,
  kExit = 4,
};

class HostFnErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "hostfn"; }
  std::string message(int code) const override;
};

struct HostFnError : public EnumError<HostFnErrorCode, HostFnErrorCategory> {
  explicit HostFnError(HostFnErrorCode err) : EnumError(err) {}
};

// Enable comparisons between HostFnErrorCode and std::error_code.
inline bool
operator==(const HostFnErrorCode &lhs, const std::error_code &rhs)
{
  return HostFnError{lhs} == rhs;
}

template<typename T> using HostFnStatus = std::expected<T, HostFnError>;

class InvokeErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "invoke"; }
  std::string message(int code) const override;
};

struct InvokeError : public EnumError<InvokeErrorCode, InvokeErrorCategory> {
  explicit InvokeError(InvokeErrorCode err) : EnumError(err) {}
};

// Enable comparisons between InvokeErrorCode and std::error_code.
inline bool
operator==(const InvokeErrorCode &lhs, const std::error_code &rhs)
{
  return InvokeError{lhs} == rhs;
}

template<typename T> using InvokeStatus = std::expected<T, InvokeError>;

using HostFnResult = std::pair<HostFnErrorCode, int32_t>;

} // namespace hogd
