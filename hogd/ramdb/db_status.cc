#include "hogd/hogdb/db_status.h"

#include <cassert>
#include <string>

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
  case kIO:
    return "Input/Output Error";
  default:
    return "Unknown Code";
  }
}

} // namespace hogd
