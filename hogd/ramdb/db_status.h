// db_status.h - error codes and return values for the database

#pragma once

#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"

namespace hogd {

// DBItem holds an item's value and its type. It is used as a return value.
struct DBItem {
  uint8_t type;
  OwningSpan<const std::byte> value;
};

// DBCode is an enum of database error codes.
enum class DBCode : uint8_t {
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
};

// DBCategory provides an error category for database errors.
class DBCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "db"; }
  std::string message(int code) const override;
};

// DBError is the error object for database errors.
using DBError = EnumError<DBCode, DBCategory>;

// to_db_error converts an Errno to a DBError.
DBError to_db_error(Errno e);

// Enable comparisons between DBCode and std::error_code.
inline bool
operator==(const DBCode &lhs, const std::error_code &rhs)
{
  return DBError{lhs} == rhs;
}

// DBStatus is the return type for a function that can return a database error.
template<typename T> using DBStatus = std::expected<T, DBError>;

} // namespace hogd
