#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/crypto/crypto_error.h"

namespace hogd {

using Siphash24Key = std::array<uint8_t, 16>;

/**
 * Keyed (random) hash function (SipHash-2-4)
 * Output length is 64 bits.
 * (There is a longer, 128-bit version we could use, if necessary).
 */
CryptoStatus<uint64_t>
siphash24(std::span<const std::byte, std::dynamic_extent> const &input,
          Siphash24Key const &key);

CryptoStatus<Siphash24Key> siphash24_keygen();

} // namespace hogd
