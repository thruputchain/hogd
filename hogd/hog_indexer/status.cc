#include "hogd/hog_indexer/status.h"

#include <cassert>
#include <string>

namespace hogd {

std::string
IndexerErrorCategory::message(int code) const
{
  assert(IndexerError::is_in_enum_range(code));
  using enum IndexerErrorCode;
  switch (static_cast<IndexerErrorCode>(code)) {
  case kNone:
    return "None";
  case kInternal:
    return "Internal";
  case kDBNotOpen:
    return "DB Not Open";
  case kDBAlreadyExists:
    return "DB Already Exists";
  case kDBNotFound:
    return "DB Not Found";
  case kUnsupportedCompressionAlgorithm:
    return "Unsupported Compression Algorithm";
  }
  return "Unknown code: " + std::to_string(code);
}

} // namespace hogd
