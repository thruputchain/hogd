#pragma once

#include <expected>
#include <string>
#include <system_error>

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

// Result of one host function invocation. Most host functions will return a
// HostFnStatus<SyscallErrorCode>, which is unwrapped to this result type.
// However, the kExit host function return code also needs an exit code
// (not just kNoneOrRecoverable), so we need this extra type.
// Host functions consume some amount of gas, which is not recorded
// within this object.
using HostFnResult = std::pair<HostFnErrorCode, int32_t>;

// If true, syscall handler will print args as unformatted uint64s
inline bool
is_host_fn_parse_error(HostFnResult const &res)
{
  return res.first == HostFnErrorCode::kNoneOrRecoverable &&
         res.second == static_cast<int32_t>(SyscallErrorCode::kFault);
}

// Total result of one contract invocation.
//
// A frame is successful (non-reverting) if the result_ is an expected
// (int32_t) and that value is nonnegative, while the entire transaction is
// successful if result_ is ane expected with value 0. Positive values are
// converted to a kDeterministic invoke error at the root of a transaction.
//
// Rationale: Each contract's return value is effectively a Result<Value,
// i32>, or a trap() (InvokeError::kDeterministic). A result return
// (through either the return syscall, the method returning, or an Exit
// syscall) with a nonnegative error code does not revert the function. Only
// a deterministic trap() (such as out of gas, or divide by 0), or a negative
// error code, will cause a contract to be reverted.
//
// The default behavior for a contract can be to propagate unhandled error
// codes back to the caller.  This makes the revert behavior(revert or not)
// also propagate in a sensible default way.
struct InvokeResult {
  InvokeStatus<int32_t> result_;
  uint64_t gas_consumed_;

  bool is_frame_success() const
  {
    return (result_.has_value() && *result_ >= 0);
  }
  bool should_frame_revert() const { return !is_frame_success(); }

  // TODO(geoff): check that kReturn and kExit from a root function can return
  // successfully
  bool is_root_success() const { return result_.has_value() && *result_ == 0; }

  InvokeResult to_root_error() const;
  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t> to_host_fn_result() const;
};

} // namespace hogd
