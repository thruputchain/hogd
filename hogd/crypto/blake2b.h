#pragma once

#include <array>
#include <cstdint>

namespace hog {

namespace detail {
void __attribute__((constructor)) initialize_sodium();
}

// waiting on XDR
using Hash = std::array<uint8_t, 32>;

void hash_blake2b(Hash &out, const uint8_t *data_in, uint32_t data_in_len);

} // namespace hog
