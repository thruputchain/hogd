// status.h - error codes and return values for hog_db
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

// HogDbCode is an enum of hog_db error codes.
enum class HogDbCode : uint8_t {
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
  // After this point, these are all unique HogDbCodes
  //

  // Integer overflow.
  kOverflow,

  // A transaction conflict occurred.
  kConflict,

  // The executor produced a delta that violates internal invariants.
  // This can only happen with a dishonest proposer or when replaying
  // a dishonest block; an honest executor never triggers this.
  kInvalidExecutorBehavior,

  // The block snapshot is stale — a sibling block committed and
  // changed state that this block already read. The block should
  // be abandoned.
  kStaleBlock,
};

// HogDbCategory provides an error category for hog_db errors.
class HogDbCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "hog_db"; }
  std::string message(int code) const override;
};

// HogDbError is the error object for hog_db errors.
using HogDbError = EnumError<HogDbCode, HogDbCategory>;

// to_db_error converts an DBError to a HogDbError.
inline HogDbError
to_hog_db_error(DBError err)
{
  assert(err.raw_code() <= std::to_underlying(HogDbCode::kCorruption));
  return HogDbError(static_cast<HogDbCode>(err.raw_code()));
}

// Enable comparisons between HogDbCode and std::error_code.
inline bool
operator==(const HogDbCode &lhs, const std::error_code &rhs)
{
  return HogDbError{lhs} == rhs;
}

// HogDbStatus is the return type for a function that can return a hog_db error.
template<typename T> using HogDbStatus = std::expected<T, HogDbError>;

} // namespace hogd
