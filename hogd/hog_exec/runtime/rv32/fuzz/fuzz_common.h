#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_program.h"

// Pure, header-only helpers shared by the rv32 fuzz targets: deterministic
// input carving and decoder-parity scans. No fuzzer/sanitizer dependency, so a
// plain gtest can unit-test the logic. Kept out of the production headers
// because it is harness-only.
namespace hogd::rv32::fuzz {

inline constexpr uint64_t kFuzzGasCap = 2048;
inline constexpr uint32_t kFuzzMemCap = 4096;
// Run length <= gas budget (every instruction costs >= 1 gas), so a budget
// below the traced step window guarantees the canonical trace_execution reaches
// a real terminal instead of a synthetic truncation kTrap that the uncapped
// threaded TraceObserver would never produce.
inline constexpr std::size_t kFuzzTraceMaxSteps = kFuzzGasCap + 64;
static_assert(kFuzzGasCap < kFuzzTraceMaxSteps);
static_assert(kFuzzMemCap <= kMaxMemoryBytes);

// Consume-from-front little-endian reader. Reads past the end yield zero bytes
// (never over-read), so a short fuzzer input degrades gracefully into zeros.
class ByteReader {
public:
  explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

  uint32_t u32()
  {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= uint32_t{next()} << (i * 8);
    }
    return value;
  }

  uint64_t u64()
  {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= uint64_t{next()} << (i * 8);
    }
    return value;
  }

  [[nodiscard]] std::span<const uint8_t> rest() const
  {
    return data_.subspan(pos_);
  }
  [[nodiscard]] std::size_t remaining() const { return data_.size() - pos_; }

private:
  uint8_t next() { return pos_ < data_.size() ? data_[pos_++] : uint8_t{0}; }

  std::span<const uint8_t> data_;
  std::size_t pos_ = 0;
};

// First decode error over `code` per the canonical decoder, or nullopt if the
// whole stream decodes. This is Program::validate's size/alignment checks plus
// its in-order per-word decode.
inline std::optional<ProgramError>
canonical_scan_error(std::span<const uint8_t> code)
{
  const ProgramResult result = Program::validate(code);
  if (result.ok) {
    return std::nullopt;
  }
  return result.error;
}

// First decode error over `code` per the threaded decoder, or nullopt. Note
// ThreadedProgram::create only validates size/alignment; opcode validation is
// in instruction_at, so every instruction must be scanned to match the
// canonical decoder's coverage.
inline std::optional<ProgramError>
threaded_scan_error(std::span<const uint8_t> code)
{
  const auto program = ThreadedProgram::create(code);
  if (!program.has_value()) {
    return program.error();
  }
  for (std::size_t i = 0; i < program->instruction_count(); ++i) {
    const auto instruction = program->instruction_at(i);
    if (!instruction.has_value()) {
      return instruction.error();
    }
  }
  return std::nullopt;
}

struct EngineDiffInput {
  uint64_t gas_budget = 0;
  uint32_t memory_bytes = 0;
  std::vector<uint8_t> code; // non-empty, multiple of kInstructionBytes
};

// Layout: [u64 gas][u32 memory_bytes][code bytes...]. gas/mem are capped; the
// tail is truncated down to whole instructions. nullopt if no full word
// remains.
inline std::optional<EngineDiffInput>
carve_engine_diff_input(std::span<const uint8_t> data)
{
  ByteReader reader(data);
  const uint64_t gas = reader.u64() % (kFuzzGasCap + 1);
  const uint32_t mem = reader.u32() % (kFuzzMemCap + 1);
  const std::span<const uint8_t> tail = reader.rest();
  const std::size_t words = tail.size() / kInstructionBytes;
  if (words == 0) {
    return std::nullopt;
  }
  EngineDiffInput in;
  in.gas_budget = gas;
  in.memory_bytes = mem;
  in.code.assign(tail.begin(), tail.begin() + words * kInstructionBytes);
  return in;
}

struct ArtifactFixup {
  uint32_t initial_memory_start = 0;
  uint32_t memory_bytes = 0;
  std::vector<uint8_t> initial_memory;
  std::vector<uint32_t> code_words; // non-empty
};

// Layout: [u32 start][u32 mem_len][u32 extra][payload...]. The payload is split
// into initial_memory (clamped to fit both kMaxMemoryBytes and the remaining
// bytes) followed by code. When the payload can hold at least one instruction,
// reserve one word for code instead of letting the image consume the whole
// tail. The empty initial-memory image has a single canonical start encoding
// (0), and memory_bytes is floor plus fuzzer-chosen headroom to exercise the
// zero-extended tail. nullopt if the payload is empty.
inline std::optional<ArtifactFixup>
carve_artifact_fixup(std::span<const uint8_t> data)
{
  ByteReader reader(data);
  const uint32_t start = reader.u32() % (kMaxMemoryBytes + 1);
  const uint32_t mem_len_knob = reader.u32();
  const uint32_t extra_knob = reader.u32();
  const std::span<const uint8_t> tail = reader.rest();
  if (tail.empty()) {
    return std::nullopt;
  }

  const uint32_t max_by_space = kMaxMemoryBytes - start;
  const auto max_by_buffer = static_cast<uint32_t>(
      tail.size() >= kInstructionBytes ? tail.size() - kInstructionBytes
                                       : tail.size());
  const uint32_t max_mem_len = std::min(max_by_space, max_by_buffer);
  const uint32_t mem_len =
      max_mem_len == 0 ? 0U : mem_len_knob % (max_mem_len + 1);

  ArtifactFixup fixup;
  fixup.initial_memory_start = start;
  fixup.initial_memory.assign(tail.begin(), tail.begin() + mem_len);
  if (fixup.initial_memory.empty()) {
    fixup.initial_memory_start = 0;
  }

  const std::span<const uint8_t> code_tail = tail.subspan(mem_len);
  for (std::size_t i = 0; i + kInstructionBytes <= code_tail.size();
       i += kInstructionBytes) {
    fixup.code_words.push_back(load_u32_le(code_tail, i));
  }
  if (fixup.code_words.empty()) {
    fixup.code_words.push_back(test_program::addi(1, 0, 0));
  }

  const uint32_t floor = fixup.initial_memory_start +
                         static_cast<uint32_t>(fixup.initial_memory.size());
  const uint32_t min_memory = std::max<uint32_t>(1, floor);
  const uint32_t room = kMaxMemoryBytes - min_memory;
  fixup.memory_bytes = min_memory + (room == 0 ? 0U : extra_knob % (room + 1));
  return fixup;
}

} // namespace hogd::rv32::fuzz
