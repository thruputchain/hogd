#include "hogd/ramdb/db_status.h"

#include <cassert>
#include <cerrno>
#include <string>

#include "hogd/base/status.h"

namespace hogd {

std::string
DBCategory::message(int code) const
{
  assert(DBError::is_in_enum_range(code));

  using enum DBCode;
  switch (static_cast<DBCode>(code)) {
  case kNotFound:
    return "Not Found";
  case kTypeMismatch:
    return "Type Mismatch";
  case kTooBig:
    return "Too Big";
  case kNoMemory:
    return "Out of Memory";
  case kInternal:
    return "Internal Error";
  case kIO:
    return "Input/Output Error";
  default:
    return "Unknown Code";
  }
}

DBError
to_db_error(Errno e)
{
  using enum DBCode;
  switch (e.raw_code()) {
  case ENOENT:
  case EISDIR:
  case ENOTDIR:
    return DBError(kNotFound);
  case ENOMEM:
    return DBError(kNoMemory);
  case EIO:
    return DBError(kIO);
  default:
    return DBError(kInternal);
  }
}

} // namespace hogd
