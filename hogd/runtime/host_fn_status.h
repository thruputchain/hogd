#pragma once

#include <expected>

#include "hogd/base/status.h"

namespace hogd {

<<<<<<< HEAD

/*
enum WasmiInvokeError {
  None = 0,
  LinkError = 1, // module requests fn that was not linked
  FuncNExist = 2,
  InputError = 3, // input validation fails
  ReturnTypeError = 4,
  CallError = 5, // TODO(geoff) confirm, but I believe this means the wasm internall hit an `unreachable`
  HostError = 6,
  UnrecoverableSystemError = 7,
} */
// NOLINTNEXTLINE(performance-enum-size)
enum class WasmiInvokeError : uint32_t {
  kWasmiNone = 0,
  kWasmiLinkError = 1,
  kWasmiFnNExist = 2,
  kWasmiInputError = 3,
  kWasmiReturnTypeError = 4,
  kWasmiCallError = 5,
  kWasmiHostError = 6,
  kWasmiUnrecoverableSystemError = 7
};

=======
>>>>>>> ff18635 (initial draft wasmi runtime no invoke() or wat2wasm yet)
// Host fn calls have two types of errors:
// 1) Recoverable (possibly handled by the caller contract,
// must generate deterministically -- e.g. accessing a nonexistent key)
// 2) Everything else (Unrecoverable)
<<<<<<< HEAD
// We must be very deliberate about what errors we mark in the first category.
enum class HostFnErrorCode : uint8_t {
  kUnrecoverableUnknown = 1,
  kRecoverableUnknown = 2,
  // replicating WasmiInvokeError
  kWasmiLinkError = 3,
  kWasmiFnNExist = 4,
  kWasmiInputError = 5,
  kWasmiReturnTypeError = 6,
  kWasmiCallError = 7,
  kWasmiHostError = 8,
  kWasmiUnrecoverableSystemError = 9
};

HostFnErrorCode wasmi_to_host_fn_error(WasmiInvokeError wasmi_error);

=======
enum class HostFnErrorCode : uint8_t {
  kUnrecoverableUnknown = 1,
  kRecoverableUnknown = 2,
};

>>>>>>> ff18635 (initial draft wasmi runtime no invoke() or wat2wasm yet)
class HostFnErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "hostfn"; }
  std::string message(int code) const override;
};

struct HostFnError : public EnumError<HostFnErrorCode, HostFnErrorCategory> {
  explicit HostFnError(HostFnErrorCode err) : EnumError(err) {}

  bool recoverable() const;
};

// Enable comparisons between HostFnErrorCode and std::error_code.
inline bool
operator==(const HostFnErrorCode &lhs, const std::error_code &rhs)
{
  return HostFnError{lhs} == rhs;
}

template<typename T> using HostFnStatus = std::expected<T, HostFnError>;

} // namespace hogd
