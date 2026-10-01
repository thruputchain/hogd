#pragma once

#include <string>
#include <string_view>

namespace hogd {

// Converts jemalloc heap_v2 dumps to an uncompressed profile.proto message.
// maps must be Linux /proc/self/maps from the process that produced the dump.
// Throws std::invalid_argument for malformed input. No symbolization is done.
std::string jemalloc_to_pprof(std::string_view dump, std::string_view maps);

} // namespace hogd
