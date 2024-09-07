#pragma once

#include <cstdint>

namespace hogd {

constexpr static uint64_t kSyscallGetBlockNumberCost   = 1;
constexpr static uint64_t kSyscallGetPrevBlockHashCost = 10;

constexpr static auto kHashCost = [](uint64_t bytes_len) -> uint64_t {
  return bytes_len * 100 + 500;
};

constexpr static uint64_t kSyscallProxyLookupFoundCost = 1;
constexpr static uint64_t kSyscallProxyLookupNewCost   = 100;

} // namespace hogd
