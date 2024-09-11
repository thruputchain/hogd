#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/crypto/crypto_error.h"

namespace hogd {

// TODO(geoff): replace when x23 ready
using Hash = std::array<uint8_t, 32>;

/**
 * SHA256
 *
 * libsodium supports a multipart hashing API, should we need
 * hashes of very long inputs.
 */
CryptoStatus<Hash>
sha256(std::span<const std::byte, std::dynamic_extent> const &input);

} // namespace hogd
