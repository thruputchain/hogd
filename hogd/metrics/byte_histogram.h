#pragma once

#include <array>
#include <vector>

namespace hogd {

// Powers of two from 1 KiB through 512 MiB. 2 MiB and 4 MiB are exact edges:
// firehose drops a record above its 4 MiB default, and 2 MiB is the step
// before that.
inline constexpr std::array<double, 20> kPowerOfTwoBytes = {
    1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15, 1 << 16,
    1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21, 1 << 22, 1 << 23,
    1 << 24, 1 << 25, 1 << 26, 1 << 27, 1 << 28, 1 << 29,
};

// Histogram upper bounds for byte sizes.
inline const std::vector<double>
    power_of_two_byte_bounds(kPowerOfTwoBytes.begin(), kPowerOfTwoBytes.end());

} // namespace hogd
