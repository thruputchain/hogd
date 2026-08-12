#include "hogd/hog_event/status.h"

#include <cassert>
#include <string>

namespace hogd {

std::string
EventErrorCategory::message(int code) const
{
  assert(EventError::is_in_enum_range(code));
  using enum EventErrorCode;
  switch (static_cast<EventErrorCode>(code)) {
  case kNone:
    return "None";
  case kBadBlockID:
    return "Bad Block ID";
  case kExists:
    return "Exists";
  case kInternal:
    return "Internal";
  }
  return "Unknown code: " + std::to_string(code);
}

} // namespace hogd
