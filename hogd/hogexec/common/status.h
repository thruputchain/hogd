#pragma once

#include <cassert>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "syscall_error.hh"

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
  constexpr explicit SyscallError(SyscallErrorCode err) : EnumError(err) {}
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

struct SystemError : public EnumError<SystemErrorCode, SystemErrorCategory> {
  constexpr explicit SystemError(SystemErrorCode err) : EnumError(err) {}

  static SystemError from_raw_error_code(int32_t raw_error_code)
  {
    switch (raw_error_code) {
    case std::to_underlying(SystemErrorCode::Unrecoverable):
      return SystemError{SystemErrorCode::Unrecoverable};
    case std::to_underlying(SystemErrorCode::Abandon):
      return SystemError{SystemErrorCode::Abandon};
    case std::to_underlying(SystemErrorCode::ValidationFailure):
      return SystemError{SystemErrorCode::ValidationFailure};
    default:
      error("Unrecoverable: Unknown system error code: {}", raw_error_code);
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  }

  HostFnError to_host_fn_error() const
  {
    switch (code()) {
    case SystemErrorCode::Unrecoverable:
      return HostFnError{HostFnErrorCode::Unrecoverable};
    case SystemErrorCode::Abandon:
      return HostFnError{HostFnErrorCode::Abandon};
    case SystemErrorCode::ValidationFailure:
      return HostFnError{HostFnErrorCode::ValidationFailure};
    }
    panic("invalid SystemErrorCode in to_host_fn_error: {}",
          std::to_underlying(code()));
  }

  InvokeError to_invoke_error() const
  {
    switch (code()) {
    case SystemErrorCode::Unrecoverable:
      return InvokeError{InvokeErrorCode::Unrecoverable};
    case SystemErrorCode::Abandon:
      return InvokeError{InvokeErrorCode::Abandon};
    case SystemErrorCode::ValidationFailure:
      return InvokeError{InvokeErrorCode::ValidationFailure};
    }
    panic("invalid SystemErrorCode in to_invoke_error: {}",
          std::to_underlying(code()));
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

  constexpr bool compatible_with(InvokeResult const &other) const
  {
    // We can chain one InvokeResult on to another if the first was successful,
    // or they both indicate the same error.
    return is_frame_success() ||
           (!other.is_frame_success() &&
            other.result_.error().code() == result_.error().code());
  }

  InvokeResult &operator+=(InvokeResult const &other)
  {
    assert(compatible_with(other));
    gas_consumed_ += other.gas_consumed_;
    result_ = other.result_;
    return *this;
  }

  constexpr bool is_frame_success() const { return (result_.has_value()); }
  constexpr bool should_frame_revert() const { return !is_frame_success(); }

  constexpr bool is_root_success() const { return is_frame_success(); }

  bool has_readable_proposal_data() const
  {
    return result_.has_value() ||
           result_.error().code() == InvokeErrorCode::Deterministic;
  }

  std::pair<SystemStatus<SyscallErrorCode>, uint64_t> to_host_fn_result() const;
};

} // namespace hogd
