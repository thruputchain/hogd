#pragma once

#include <expected>

#include "hogd/base/status.h"

#include "hogd/syscall/syscall_error_code.h"

namespace hogd {

class SyscallErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "syscall"; }
  std::string message(int code) const override;
};

struct SyscallError : public EnumError<SyscallErrorCode, SyscallErrorCategory> {
  explicit SyscallError(SyscallErrorCode err) : EnumError(err) {}

  // Can the error be passed back to the smart contract
  bool recoverable() const;
};

// Enable comparisons between SyscallErrorCode and std::error_code.
inline bool
operator==(const SyscallErrorCode &lhs, const std::error_code &rhs)
{
  return SyscallError{lhs} == rhs;
}

template<typename T> using SyscallStatus = std::expected<T, SyscallError>;

} // namespace hogd
