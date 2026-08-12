#include "hogd/hog_db/status.h"

#include <cassert>
#include <string>
#include <utility>

#include "hogd/ramdb/db_status.h"

namespace hogd {

namespace {

constexpr bool
is_same(HogDbCode a, DBCode b)
{
  return std::to_underlying(a) == std::to_underlying(b);
}

static_assert(is_same(HogDbCode::kNotFound, DBCode::kNotFound));
static_assert(is_same(HogDbCode::kTypeMismatch, DBCode::kTypeMismatch));
static_assert(is_same(HogDbCode::kTooBig, DBCode::kTooBig));
static_assert(is_same(HogDbCode::kNoMemory, DBCode::kNoMemory));
static_assert(is_same(HogDbCode::kInternal, DBCode::kInternal));
static_assert(is_same(HogDbCode::kIO, DBCode::kIO));
static_assert(is_same(HogDbCode::kCorruption, DBCode::kCorruption));

} // namespace

std::string
HogDbCategory::message(int code) const
{
  assert(HogDbError::is_in_enum_range(code));

  using enum HogDbCode;
  switch (static_cast<HogDbCode>(code)) {
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
  case kCorruption:
    return "Data Corruption";
  case kOverflow:
    return "Overflow";
  case kConflict:
    return "Conflict";
  case kExists:
    return "Exists";
  case kInvalidExecutorBehavior:
    return "Invalid Executor Behavior";
  case kStaleBlock:
    return "Stale Block";
  default:
    return "Unknown Code (" + std::to_string(code) + ")";
  }
}

} // namespace hogd
