#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

namespace hogd::rv32::test_artifact {

inline void
append_bytes(std::vector<uint8_t> &out, std::span<const uint8_t> bytes)
{
  out.insert(out.end(), bytes.begin(), bytes.end());
}

inline void
append_u32(std::vector<uint8_t> &out, uint32_t value)
{
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

inline void
append_u64(std::vector<uint8_t> &out, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    out.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

inline void
append_digest(std::vector<uint8_t> &out, ArtifactDigest const &digest)
{
  out.insert(out.end(), digest.begin(), digest.end());
}

// Assemble a v5 artifact: header || code || initial_memory, with the stored
// image placed at data-space VA `initial_memory_start`. memory_bytes is
// stamped as start + stored size (callers that want a larger data address
// space rewrite the memory_bytes header field, exercising the zero-extended
// tail path).
inline std::vector<uint8_t>
build_artifact(std::span<const uint32_t> words,
               std::span<const uint8_t> initial_memory,
               uint32_t initial_memory_start = 0)
{
  const std::vector<uint8_t> code = test_program::code_from_words(words);

  std::vector<uint8_t> artifact;
  artifact.reserve(kArtifactHeaderBytes + code.size() + initial_memory.size());
  append_bytes(artifact, kArtifactMagic);
  append_u32(artifact, kArtifactFormatVersion);
  append_u32(artifact, kArtifactHeaderBytes);
  append_u64(artifact, kArtifactVmProfileDigest);
  append_u32(artifact, 0);
  append_u32(artifact, kArtifactEntryAbiVersion);
  append_u32(artifact, kArtifactSyscallAbiVersion);
  append_u32(artifact, kGasTableVersion);
  append_u64(artifact, kGasScheduleDigest);
  append_u32(artifact, initial_memory_start +
                           static_cast<uint32_t>(initial_memory.size()));
  append_u32(artifact, static_cast<uint32_t>(code.size()));
  append_u32(artifact, static_cast<uint32_t>(initial_memory.size()));
  append_u32(artifact, initial_memory_start);
  append_digest(artifact, artifact_digest(code));
  append_digest(artifact, artifact_digest(initial_memory));
  append_bytes(artifact, code);
  append_bytes(artifact, initial_memory);
  return artifact;
}

inline std::vector<uint8_t>
build_artifact(std::initializer_list<uint32_t> words,
               std::span<const uint8_t> initial_memory,
               uint32_t initial_memory_start = 0)
{
  return build_artifact(std::span<const uint32_t>(words.begin(), words.size()),
                        initial_memory, initial_memory_start);
}

} // namespace hogd::rv32::test_artifact
