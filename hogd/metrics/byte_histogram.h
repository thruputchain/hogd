#pragma once

#include <array>
#include <vector>

namespace hogd {

// Powers of two from 1 KiB through 512 MiB. 2 MiB and 4 MiB are exact edges:
// firehose drops a record above its 4 MiB default, and 2 MiB is the step
// before that.
inline constexpr std::array<double, 20> kPowerOfTwoByteBounds = {
    1024,     2048,     4096,     8192,      16384,     32768,     65536,
    131072,   262144,   524288,   1048576,   2097152,   4194304,   8388608,
    16777216, 33554432, 67108864, 134217728, 268435456, 536870912,
};

inline std::vector<double>
power_of_two_byte_bounds()
{
  return {kPowerOfTwoByteBounds.begin(), kPowerOfTwoByteBounds.end()};
}

} // namespace hogd
