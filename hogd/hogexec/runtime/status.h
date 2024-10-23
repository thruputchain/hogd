#pragma once

#include <expected>

#include "hogd/base/status.h"

namespace hogd {

// Returned from a runtime (wasmi) invocation.
// This is the external interface to a smart contract.
enum class InvokeErrorCode : uint8_t {
  kNone = 0,
  kDeterministic = 1,
  kOutOfGas = 2,
  kUnrecoverable = 3,
};

// Error codes returned from a host function call (from within a wasmi
// invocation). Host functions return a HostFnStatus<int32_t> (see caveat
// below), where the int32_t is a (defined elsewhere) error code from the host
// function. If a nonzero value is returned via the int32_t, then the host
// function error is recoverable, and the wasmi invocation should continue
// (possibly handling the error).
// If the host function returns a nonzero HostFnErrorCode, then
// invocation ceases and returns to the caller of the wasmi invocation.
// kExit and kReturn do not signal semantic "error", but still terminate
// invocation.
//
// When a Wasmi invocation is invoked from within a wasmi invocation
// (e.g., a contract calls another contract), kExit and kReturn are handled
// differently. kReturn surfaces a byte buffer to be returned to the caller, and
// results in the host function (at the caller) returning
// {InvokeErrorCode::kNone, 0}. kExit does not surface a byte buffer, and
// results in the host function returning {InvokeErrorCode::kNone, error code}.
// At the root (for a transaction) wasmi invocation, a nonzero error code
// associated with a returned InvokeStatus is converted to an
// InvokeErrorCode::kDeterministic.
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
