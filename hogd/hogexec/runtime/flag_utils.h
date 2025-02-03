#pragma once

#include "types.hh"

namespace hogd {

constexpr static bool
is_valid_storage_delta_flag(StorageDeltaFlags flags)
{
  constexpr static int32_t kValidFlags =
      std::to_underlying(StorageDeltaModifier::kLockExclusive);
  return ((~kValidFlags) & flags) == 0;
}

constexpr static bool
is_valid_invoke_flag(InvocationFlagSet flags)
{
  constexpr static int32_t kValidFlags =
      std::to_underlying(InvocationFlags::kTryInvoke);
  return ((~kValidFlags) & flags) == 0;
}

} // namespace hogd
