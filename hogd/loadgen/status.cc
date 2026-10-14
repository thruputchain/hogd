#include "hogd/loadgen/status.h"

#include <cassert>
#include <string>

#include "loadgen.hh"
#include "x23/format.h"

namespace hogd {

std::string
LoadgenErrorCategory::message(int code) const
{
  assert(LoadgenError::is_in_enum_range(code));
  return x23::show(static_cast<LoadgenRPCError>(code));
}

} // namespace hogd
