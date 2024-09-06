#pragma once

#include <expected>

#include "hogd/base/status.h"

namespace hogd {

// Host fn calls have two types of errors:
// 1) Recoverable (possibly handled by the caller contract,
// must generate deterministically -- e.g. accessing a nonexistent key)
// 2) Everything else (Unrecoverable)
enum class HostFnErrorCode : uint8_t {
  kUnrecoverableUnknown = 1,
  kRecoverableUnknown = 2,
};

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
