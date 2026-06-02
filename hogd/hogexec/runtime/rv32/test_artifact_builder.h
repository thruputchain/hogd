#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/dispatch.h"
#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/program.h"
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

inline std::vector<uint8_t>
dispatch_bytes(std::span<const DispatchEntry> dispatch_table)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(dispatch_table.size() * kDispatchEntryBytes);
  for (const DispatchEntry &entry : dispatch_table) {
    append_u64(bytes, entry.method_id);
    append_u32(bytes, entry.pc);
    append_u32(bytes, 0);
  }
  return bytes;
}

inline std::vector<uint8_t>
build_artifact(std::span<const uint32_t> words,
               std::span<const uint8_t> initial_memory,
               std::span<const DispatchEntry> dispatch_table)
{
  const std::vector<uint8_t> code = test_program::code_from_words(words);
  const std::vector<uint8_t> dispatch = dispatch_bytes(dispatch_table);
  const uint32_t code_offset = kArtifactHeaderBytes;
  const uint32_t dispatch_offset =
      code_offset + static_cast<uint32_t>(code.size());
  const uint32_t memory_offset =
      dispatch_offset + static_cast<uint32_t>(dispatch.size());

  std::vector<uint8_t> artifact;
  artifact.reserve(kArtifactHeaderBytes + code.size() + dispatch.size() +
                   initial_memory.size());
  append_bytes(artifact, kArtifactMagic);
  append_u32(artifact, kArtifactFormatVersion);
  append_u32(artifact, kArtifactHeaderBytes);
  append_u64(artifact, kArtifactVmProfileDigest);
  append_u32(artifact, 0);
  append_u32(artifact, kArtifactEntryAbiVersion);
  append_u32(artifact, kArtifactSyscallAbiVersion);
  append_u32(artifact, kGasTableVersion);
  append_u64(artifact, kGasScheduleDigest);
  append_u32(artifact, static_cast<uint32_t>(initial_memory.size()));
  append_u32(artifact, static_cast<uint32_t>(code.size()));
  append_u32(artifact, static_cast<uint32_t>(initial_memory.size()));
  append_u32(artifact, code_offset);
  append_u32(artifact, memory_offset);
  append_u32(artifact, dispatch_offset);
  append_u32(artifact, static_cast<uint32_t>(dispatch.size()));
  append_digest(artifact, artifact_digest(code));
  append_digest(artifact, artifact_digest(initial_memory));
  append_digest(artifact, artifact_digest(dispatch));
  append_bytes(artifact, code);
  append_bytes(artifact, dispatch);
  append_bytes(artifact, initial_memory);
  return artifact;
}

inline std::vector<uint8_t>
build_artifact(std::initializer_list<uint32_t> words,
               std::span<const uint8_t> initial_memory,
               std::span<const DispatchEntry> dispatch_table)
{
  return build_artifact(std::span<const uint32_t>(words.begin(), words.size()),
                        initial_memory, dispatch_table);
}

} // namespace hogd::rv32::test_artifact
