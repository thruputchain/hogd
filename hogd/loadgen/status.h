#pragma once

#include <cassert>
#include <expected>
#include <string>
#include <system_error>

#include "hogd/base/status.h"
#include "loadgen.hh"

namespace hogd {

class LoadgenErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "loadgen"; }
  std::string message(int code) const override;
};

struct LoadgenError : public EnumError<LoadgenRPCError, LoadgenErrorCategory> {
  explicit LoadgenError(LoadgenRPCError err) : EnumError(err) {}
};

// Enable comparisons between LoadgenRPCError and std::error_code.
inline bool
operator==(const LoadgenRPCError &lhs, const std::error_code &rhs)
{
  return LoadgenError{lhs} == rhs;
}

template<typename T> using LoadgenStatus = std::expected<T, LoadgenError>;

} // namespace hogd
