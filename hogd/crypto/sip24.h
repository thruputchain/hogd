#pragma once

#include <array>
#include <cstdint>

namespace hog {

using Sip24Key = std::array<uint8_t, 16>;

uint64_t sip24(const uint8_t *data_in, uint32_t data_in_lenk,
               Sip24Key const &key);

Sip24Key sip24_keygen();

} // namespace hog
