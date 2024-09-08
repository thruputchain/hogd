#pragma once

#include <cstdint>

#include "hogd/syscall/metered_result.h"

namespace hogd {

constexpr static GasAmount kSyscallGetBlockNumberCost   = 1_gas;
constexpr static GasAmount kSyscallGetPrevBlockHashCost = 10_gas;

constexpr static auto kHashCost = [](uint64_t bytes_len) -> GasAmount {
  return GasAmount{bytes_len * 100 + 500};
};

constexpr static GasAmount kSyscallProxyLookupFoundCost = 1_gas;
constexpr static GasAmount kSyscallProxyLookupNewCost   = 100_gas;

} // namespace hogd
