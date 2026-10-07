#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hogd {

struct HeapSymbol {
  std::string name;
  std::string system_name;
};

class HeapSymbolizer {
public:
  virtual ~HeapSymbolizer() = default;
  // Lowercase hex GNU build ID, or empty if unknown.
  virtual std::string build_id(const std::string &path) = 0;
  // Function containing pc - mapping.start + mapping.offset.
  virtual std::optional<HeapSymbol> symbol(const std::string &path,
                                           std::uint64_t file_offset) = 0;
};

// Converts jemalloc heap_v2 dumps to an uncompressed profile.proto message.
// maps must be Linux /proc/self/maps from the process that produced the dump.
// Resolves build IDs and function names from ELF files; no DWARF is read.
// Throws std::invalid_argument for malformed input.
std::string jemalloc_to_pprof(std::string_view dump, std::string_view maps,
                              HeapSymbolizer &symbolizer);
std::string jemalloc_to_pprof(std::string_view dump, std::string_view maps);

} // namespace hogd
