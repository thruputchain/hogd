#pragma once

#include <cassert>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "syscall.hh"

namespace hogd {

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

class SystemErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "system"; }
  std::string message(int code) const override;
};

enum class SystemErrorCode {
  // Unrecoverable error (e.g. out of memory, or critical logic error).
  // Only recourse is to abort entire transaction and possibly crash binary.
  // Signals serious error, which we should investigate.
  kUnrecoverable,
  // Nondeterministic error that occurs during normal course of operation.
  // E.g. a transaction is being executed concurrently with the whole block
  // getting cancelled.
  // Only recourse is to abort the entire transaction.
  kAbortTransaction,
  // Failure during validation of a transaction.  Signals that the whole
  // block must be aborted.
  kValidationFailure,
};

struct SystemError : public EnumError<SystemErrorCode, SystemErrorCategory> {
  explicit SystemError(SystemErrorCode err) : EnumError(err) {}

  HostFnError to_host_fn_error() const
  {
    switch (code()) {
    case SystemErrorCode::kUnrecoverable:
      return HostFnError{HostFnErrorCode::kHostUnrecoverable};
    case SystemErrorCode::kAbortTransaction:
      return HostFnError{HostFnErrorCode::kHostAbortTransaction};
    case SystemErrorCode::kValidationFailure:
      return HostFnError{HostFnErrorCode::kHostValidationFailure};
    }
    assert(false);
    std::unreachable();
  }

  InvokeError to_invoke_error() const
  {
    switch (code()) {
    case SystemErrorCode::kUnrecoverable:
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    case SystemErrorCode::kAbortTransaction:
      return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
    case SystemErrorCode::kValidationFailure:
      return InvokeError{InvokeErrorCode::kInvokeValidationFailure};
    }
    assert(false);
    std::unreachable();
  }
};

// Enable comparisons between HostFnErrorCode and std::error_code.
inline bool
operator==(const SystemErrorCode &lhs, const std::error_code &rhs)
{
  return SystemError{lhs} == rhs;
}

template<typename T> using SystemStatus = std::expected<T, SystemError>;

// Total result of one contract invocation.
//
// A frame is successful (non-reverting) if the result_ is an expected.
struct InvokeResult {
  InvokeStatus<void> result_;
  uint64_t gas_consumed_;

  constexpr bool is_frame_success() const { return (result_.has_value()); }
  constexpr bool should_frame_revert() const { return !is_frame_success(); }

  constexpr bool is_root_success() const { return is_frame_success(); }

  InvokeResult to_root_error() const;
  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t> to_host_fn_result() const;
};

} // namespace hogd
