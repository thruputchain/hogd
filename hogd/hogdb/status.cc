#include "hogd/hogdb/status.h"

#include <cassert>
#include <cerrno>
#include <string>
#include <utility>

#include "hogd/ramdb/db_status.h"

namespace hogd {

namespace {

constexpr bool
is_same(HogDBCode a, DBCode b)
{
  return std::to_underlying(a) == std::to_underlying(b);
}

static_assert(is_same(HogDBCode::kNotFound, DBCode::kNotFound));
static_assert(is_same(HogDBCode::kTypeMismatch, DBCode::kTypeMismatch));
static_assert(is_same(HogDBCode::kTooBig, DBCode::kTooBig));
static_assert(is_same(HogDBCode::kNoMemory, DBCode::kNoMemory));
static_assert(is_same(HogDBCode::kInternal, DBCode::kInternal));
static_assert(is_same(HogDBCode::kIO, DBCode::kIO));
static_assert(is_same(HogDBCode::kCorruption, DBCode::kCorruption));

} // namespace

std::string
HogDBCategory::message(int code) const
{
  assert(HogDBError::is_in_enum_range(code));

  using enum HogDBCode;
  switch (static_cast<HogDBCode>(code)) {
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
  default:
    return "Unknown Code";
  }
}

} // namespace hogd
