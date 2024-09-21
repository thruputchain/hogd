#pragma once

#include <cstdint>

namespace hogd {

/**
 * Table of gas costs for various operations.
 *
 * All of these values are placeholders.
 */
constexpr static uint64_t kSyscallGetBlockNumberCost = 1;
constexpr static uint64_t kSyscallGetPrevBlockHashCost = 10;

constexpr static auto SHA256HashCost = [](uint64_t bytes_len) -> uint64_t {
  return bytes_len * 100 + 500;
};

constexpr static uint64_t kUnknownSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

} // namespace hogd
