// status.h - error codes and return values for hog_db
//
// TODO(abelay): These codes will will be defined in XDR eventually, so need to
// figure out how to use those definitions here.

#pragma once

#include <expected>
#include <string>
#include <system_error>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

// HogDbCode is an enum of hog_db error codes.
enum class HogDbCode : uint8_t {
  //
  // These share names with DBCode; to_hog_db_error() maps each explicitly,
  // so the numeric values are independent of DBCode's.
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

  // The operation conflicts with one already in progress.
  kBusy,

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

// to_hog_db_error converts a DBError to a HogDbError. The switch has no
// default so a new DBCode fails -Wswitch until it is mapped here.
inline HogDbError
to_hog_db_error(DBError err)
{
  switch (err.code()) {
  case DBCode::kNotFound:
    return HogDbError(HogDbCode::kNotFound);
  case DBCode::kExists:
    return HogDbError(HogDbCode::kExists);
  case DBCode::kTypeMismatch:
    return HogDbError(HogDbCode::kTypeMismatch);
  case DBCode::kTooBig:
    return HogDbError(HogDbCode::kTooBig);
  case DBCode::kNoMemory:
    return HogDbError(HogDbCode::kNoMemory);
  case DBCode::kInternal:
    return HogDbError(HogDbCode::kInternal);
  case DBCode::kIO:
    return HogDbError(HogDbCode::kIO);
  case DBCode::kCorruption:
    return HogDbError(HogDbCode::kCorruption);
  case DBCode::kBusy:
    return HogDbError(HogDbCode::kBusy);
  }
  // An out-of-range DBCode is a caller bug; fail toward the generic error.
  return HogDbError(HogDbCode::kInternal);
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
