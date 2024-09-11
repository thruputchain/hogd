#pragma once

#include <expected>

#include "hogd/base/status.h"

namespace hogd {

enum class CryptoErrorCode : int8_t {
  kSiphash24Error = 1,
  kSHA256Error = 2,
  kInitializeFailure = 3,
};

class CryptoErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "crypto"; }
  std::string message(int code) const override;
};

struct CryptoError : public EnumError<CryptoErrorCode, CryptoErrorCategory> {
  explicit CryptoError(CryptoErrorCode err) : EnumError(err) {}
};

// Enable comparisons between CryptoErrorCode and std::error_code.
inline bool
operator==(const CryptoErrorCode &lhs, const std::error_code &rhs)
{
  return CryptoError{lhs} == rhs;
}

template<typename T> using CryptoStatus = std::expected<T, CryptoError>;

} // namespace hogd
