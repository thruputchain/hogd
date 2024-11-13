#pragma once

#include <expected>

#include "hogd/base/status.h"
#include "syscall.hh"

namespace hogd {

// Error codes returned from a runtime (wasmi) invocation.
// This is the external interface to a smart contract. Normally wrapped in an
// InvokeStatus<int32_t> (a std::expected).
//
// When InvokeStatus<int32_t> has an int32_t value (and an
// InvokeErrorCode::kNone), the int32_t value represents a handleable
// (deterministic) result from the invocation. (Specifically, the invoke()
// syscall in a nested smart contract invocation passes an
// {InvokeErrorCode::kNone, code} back as a
// {HostFnErrorCode::kNoneOrRecoverable, code}).
//
// This value is generated as either the returned value from the selected
// WebAssembly exported function that was run, the code caused by a host
// function returning HostFnErrorCode::kExit, or 0, if invocation ends because a
// host function returns HostFnErrorCode::kReturn.
enum class InvokeErrorCode : uint8_t {
  // A call frame can only succeed if this code is returned.
  // All others revert the call frame.
  kNone = 0,
  // An unspecified deterministic error.
  kDeterministic = 1,
  // Runtime exceeded its gas budget.
  kOutOfGas = 2,
  // Nondeterministic error during invocation (e.g. malloc error)
  kUnrecoverable = 3,
};

// HostFunctionErrorCode categorizes errors returned from system calls. The
// normal usage pattern is to wrap them within HostFnStatus<int32_t>, where the
// int32 is the more detailed error code.
//
// In WebAssembly, a host function is defined as a function that can be
// called outside of WebAssembly. We import one of these functions to provide
// access to our table of system call handlers.
//
// When a HostFnStatus<int32_t> (a wrapper around std::expected) has an int32_t
// value, (and a HostFnErrorCode::kNoneOrRecoverable), that int32_t is passed to
// WebAssembly as a handleable error code, as the return result from the host
// function. The WebAssembly bytecode (the smart contract) itself never sees the
// HostFnErrorCode. Other HostFnErrorCodes terminate the WebAssembly runtime
// (returning to the caller), and are handled by the WebAssembly runtime.
enum class HostFnErrorCode : uint8_t {
  kNoneOrRecoverable = 0,
  // Insufficient gas to continue WebAssembly execution.
  //
  // Result: the WebAssembly runtime immediately returns an
  // InvokeErrorCode::kOutOfGas.
  kOutOfGas = 1,
  // Unrecoverable (nondeterministic) error, such as a malloc error.
  // The only course of action is to abort the entire transaction.
  //
  // Result: the WebAssembly runtime immediately returns an
  // InvokeErrorCode::kUnrecoverable
  kUnrecoverable = 2,
  // Return from a WebAssembly invocation to the caller.  Registers a byte
  // buffer to be passed back to the caller.
  //
  // Result: Runtime immediately returns an {InvokeErrorCode::kNone, 0}.
  kReturn = 3,
  // Return from a WebAssembly invocation to the caller, with some (int32_t)
  // error code.
  //
  // Result: Runtime immediately returns an {InvokeErrorCode::kNone, code}.
  //
  // On a root smart contract invocation, a nonzero code is converted to a
  // InvokeErrorCode::kDeterministic.
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

class SyscallErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "syscall"; }
  std::string message(int code) const override;
};

struct SyscallError : public EnumError<SyscallErrorCode, SyscallErrorCategory> {
  explicit SyscallError(SyscallErrorCode err) : EnumError(err) {}
};

// Enable comparisons between SyscallErrorCode and std::error_code.
inline bool
operator==(const SyscallErrorCode &lhs, const std::error_code &rhs)
{
  return SyscallError{lhs} == rhs;
}

template<typename T> using SyscallStatus = std::expected<T, SyscallError>;

using HostFnResult = std::pair<HostFnErrorCode, int32_t>;

} // namespace hogd
