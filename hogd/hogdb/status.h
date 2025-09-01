// status.h - error codes and return values for hogdb
//
// TODO(abelay): These codes will will be defined in XDR eventually, so need to
// figure out how to use those definitions here.

#pragma once

#include <cassert>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

// HogDBCode is an enum of hogdb error codes.
enum class HogDBCode : uint8_t {
  //
  // All of these are the same as DBCode
  //

  // The item was not found.
  kNotFound = 0,

  // The item already exists.
  kExists,

  // The requested item type did not match the stored type.
  kTypeMismatch,

  // The item exceeds the size limit.
  kTooBig,

  // Out of memory.
  kNoMemory,

  // An internal error occurred.
  kInternal,

  // An I/O error (e.g., disk failure) occurred.
  kIO,

  // Data corruption detected.
  kCorruption,

  //
  // After this point, these are all unique HogDBCodes
  //

  // Integer overflow.
  kOverflow,

  // A transaction conflict occurred.
  kConflict,
};

// HogDBCategory provides an error category for hogdb errors.
class HogDBCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "hogdb"; }
  std::string message(int code) const override;
};

// HogDBError is the error object for hogdb errors.
using HogDBError = EnumError<HogDBCode, HogDBCategory>;

// to_db_error converts an DBError to a HogDBError.
inline HogDBError
to_hogdb_error(DBError err)
{
  assert(err.raw_code() <= std::to_underlying(HogDBCode::kCorruption));
  return HogDBError(static_cast<HogDBCode>(err.raw_code()));
}

// Enable comparisons between HogDBCode and std::error_code.
inline bool
operator==(const HogDBCode &lhs, const std::error_code &rhs)
{
  return HogDBError{lhs} == rhs;
}

// HogDBStatus is the return type for a function that can return a hogdb error.
template<typename T> using HogDBStatus = std::expected<T, HogDBError>;

} // namespace hogd
