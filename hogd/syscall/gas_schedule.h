#pragma once

#include <cstdint>

namespace hogd {

struct GasAmount {
  uint64_t gas;
  constexpr explicit GasAmount(uint64_t amt) : gas(amt) {}

  auto &operator+=(this auto &self, GasAmount const &other)
  {
    self.gas += other.gas;
    return self;
  }

  bool operator==(GasAmount const &other) const = default;
};

constexpr GasAmount
operator""_gas(uint64_t input)
{
  return GasAmount{input};
};

/**
 * Table of gas costs for various operations.
 *
 * All of these values are placeholders.
 */
constexpr static GasAmount kSyscallGetBlockNumberCost   = 1_gas;
constexpr static GasAmount kSyscallGetPrevBlockHashCost = 10_gas;

constexpr static auto kHashCost = [](uint64_t bytes_len) -> GasAmount {
  return GasAmount{bytes_len * 100 + 500};
};

constexpr static GasAmount kSyscallProxyLookupFoundCost = 1_gas;
constexpr static GasAmount kSyscallProxyLookupNewCost   = 100_gas;

} // namespace hogd
