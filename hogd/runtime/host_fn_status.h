#pragma once

#include <expected>

#include "hogd/base/status.h"

namespace hogd {


// enum WasmiInvokeError {
//  None = 0,
//  LinkError = 1, // module requests fn that was not linked
//  FuncNExist = 2,
//  InputError = 3, // input validation fails
//  ReturnTypeError = 4,
//  DeterministicCallError = 5, 
//  OutOfGasError = 6, 
//  UnrecoverableSystemError = 7,
//}

// NOLINTNEXTLINE(performance-enum-size)
enum class WasmiInvokeError : uint32_t {
  kWasmiNone = 0,
  kWasmiLinkError = 1,
  kWasmiFnNExist = 2,
  kWasmiInputError = 3,
  kWasmiReturnTypeError = 4,
  kWasmiDeterministicCallError = 5,
  kWasmiOutOfGasError = 6,
  kWasmiUnrecoverableSystemError = 7
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

} // namespace hogd
