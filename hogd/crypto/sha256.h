#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hogd {

// TODO(geoff): replace when x23 ready
using SHA256Hash = std::array<uint8_t, 32>;

/**
 * Returns SHA256 of the input span.
 *
 * Libsodium's version of sha256, on current release (1.20) does not return
 * errors.
 */
SHA256Hash sha256(std::span<const std::byte> input);

} // namespace hogd
