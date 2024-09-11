#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hogd {

using Siphash24Key = std::array<uint8_t, 16>;

/**
 * Keyed (random) hash function (SipHash-2-4)
 * Output length is 64 bits.
 * (There is a longer, 128-bit version we could use, if necessary).
 */
uint64_t siphash24(std::span<const std::byte> input, Siphash24Key const &key);

Siphash24Key siphash24_keygen();

} // namespace hogd
