#pragma once

#include <expected>

#include "hogd/base/status.h"
#include "syscall.hh"

namespace hogd {

// pub enum WasmiInvokeError {
//     None = 0,
//     FuncNExist = 1, // invoked function does not exist
//     InputError = 2, // input validation fails
//     ReturnTypeError = 3, // invoked function did not return expected type
//     DeterministicCallError = 4, // some other error, i.e. OOB access in WASM
//     OutOfGasError = 5, // out of gas
//     UnrecoverableSystemError = 6,
// }
enum class WasmiInvokeErrorCode : uint8_t {
  kWasmiNone = 0,
  kWasmiFnNExist = 1,
  kWasmiInputError = 2,
  kWasmiReturnTypeError = 3,
  kWasmiDeterministicCallError = 4,
  kWasmiOutOfGasError = 5,
  kWasmiUnrecoverableSystemError = 6
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

class WasmiInvokeErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "wasmi invoke"; }
  std::string message(int code) const override;
};

struct WasmiInvokeError
  : public EnumError<WasmiInvokeErrorCode, WasmiInvokeErrorCategory> {
  explicit WasmiInvokeError(WasmiInvokeErrorCode err) : EnumError(err) {}
};

// Enable comparisons between WasmiInvokeErrorCode and std::error_code.
inline bool
operator==(const WasmiInvokeErrorCode &lhs, const std::error_code &rhs)
{
  return WasmiInvokeError{lhs} == rhs;
}

template<typename T>
using WasmiInvokeStatus = std::expected<T, WasmiInvokeError>;

class SyscallErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "syscall"; }
  std::string message(int code) const override;
};

struct SyscallError 
  : public EnumError<SyscallErrorCode, SyscallErrorCategory> {
    explicit SyscallError(SyscallErrorCode err) : EnumError(err) {}
};

// Enable comparisons between SyscallErrorCode and std::error_code.
inline bool
operator==(const SyscallErrorCode &lhs, const std::error_code &rhs)
{
  return SyscallError{lhs} == rhs;
}

template<typename T>
using SyscallStatus = std::expected<T, SyscallError>;

} // namespace hogd
