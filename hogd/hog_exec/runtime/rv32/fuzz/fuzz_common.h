#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/rv32/threaded_program.h"

// Pure, header-only helpers shared by the rv32 fuzz targets: deterministic
// input carving and decoder-parity scans. No fuzzer/sanitizer dependency, so a
// plain gtest can unit-test the logic. Kept out of the production headers
// because it is harness-only.
namespace hogd::rv32::fuzz {

inline constexpr uint64_t kFuzzGasCap = 2048;
inline constexpr uint32_t kFuzzMemCap = 4096;
// Smallest run-gas chunk cap the carve may select. create() clamps its cap to
// at least the largest single-instruction gas so a valid split always exists;
// carving below that floor would just be silently clamped back up.
inline constexpr uint32_t kFuzzMinRunGasCap =
    static_cast<uint32_t>(kDivInstructionGas);
static_assert(kFuzzMinRunGasCap <= kMaxRunGas);
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

// Decode verdict of the PRODUCTION lowering gate over `code` alone (memory
// arguments chosen so they can never reject). The forward scan above checks
// exact first-error parity with the canonical decoder, but production
// (rv32_contract.cc) runs ThreadedMachinePreInstantiation::create, whose
// single backward lowering pass reports the LAST invalid word's error rather
// than the first. All decode errors collapse into the same deterministic
// deploy rejection, so across multiple invalid words only the accept/reject
// verdict -- not the error code -- is required to match; compare has_value(),
// not the payload.
inline std::optional<ProgramError>
production_scan_error(std::span<const uint8_t> code)
{
  const auto pre =
      ThreadedMachinePreInstantiation::create(code, /*memory=*/{},
                                              /*memory_start=*/0,
                                              /*total_memory_size=*/256,
                                              /*trace=*/false);
  if (pre.has_value()) {
    return std::nullopt;
  }
  return pre.error();
}

struct EngineDiffInput {
  uint64_t gas_budget = 0;
  uint32_t memory_bytes = 0;
  uint32_t max_run_gas = kMaxRunGas;
  uint64_t method_id = 0;    // seeded into a0/a1 (v4 guest-dispatch entry seam)
  uint64_t host_seed = 0;    // drives the ScriptedHost schedule (below)
  uint32_t memory_start = 0; // data-space VA of initial_memory
  std::vector<uint8_t> initial_memory; // seeded image; fits memory_bytes
  std::vector<uint8_t> code; // non-empty, multiple of kInstructionBytes
};

// Run-gas chunk cap for the threaded lowering, from the fuzzer-chosen knob.
// Production always runs with kMaxRunGas, but at fuzz budgets (<= kFuzzGasCap)
// a straight line long enough to split at that cap is unreachable, so break
// carriers -- and their refund and deficit seams -- would never appear. Bias
// 3/4 of inputs to a small cap (kFuzzMinRunGasCap..+63) so carriers land in
// short programs; the remaining 1/4 (including every short input, which reads
// knob 0) keep the production cap.
inline uint32_t
run_gas_cap_from_knob(uint32_t knob)
{
  if (knob % 4 == 0) {
    return kMaxRunGas;
  }
  return kFuzzMinRunGasCap + (knob / 4) % 64;
}

// Layout: [u64 gas][u32 memory_bytes][u32 run-gas-cap knob][u64 method_id]
// [u64 host_seed][u32 start_knob][u32 img_len_knob][image bytes...]
// [code bytes...]. gas/mem are capped and the cap knob is mapped by
// run_gas_cap_from_knob; start/img_len are derived so memory_start +
// initial_memory.size() <= memory_bytes always holds, the image never eats
// the last full code word (mirroring carve_artifact_fixup), and the empty
// image keeps its single canonical encoding (start 0). The code tail is
// truncated down to whole instructions; nullopt if no full word remains.
inline std::optional<EngineDiffInput>
carve_engine_diff_input(std::span<const uint8_t> data)
{
  ByteReader reader(data);
  const uint64_t gas = reader.u64() % (kFuzzGasCap + 1);
  const uint32_t mem = reader.u32() % (kFuzzMemCap + 1);
  const uint32_t cap = run_gas_cap_from_knob(reader.u32());
  const uint64_t method_id = reader.u64();
  const uint64_t host_seed = reader.u64();
  const uint32_t start_knob = reader.u32();
  const uint32_t img_len_knob = reader.u32();
  const std::span<const uint8_t> tail = reader.rest();

  const uint32_t start = mem == 0 ? 0 : start_knob % (mem + 1);
  const uint32_t max_by_space = mem - start;
  const auto max_by_buffer = static_cast<uint32_t>(
      tail.size() >= kInstructionBytes ? tail.size() - kInstructionBytes
                                       : tail.size());
  const uint32_t max_img = std::min(max_by_space, max_by_buffer);
  const uint32_t img_len = max_img == 0 ? 0U : img_len_knob % (max_img + 1);

  const std::span<const uint8_t> code_tail = tail.subspan(img_len);
  const std::size_t words = code_tail.size() / kInstructionBytes;
  if (words == 0) {
    return std::nullopt;
  }
  EngineDiffInput in;
  in.gas_budget = gas;
  in.memory_bytes = mem;
  in.max_run_gas = cap;
  in.method_id = method_id;
  in.host_seed = host_seed;
  in.initial_memory.assign(tail.begin(), tail.begin() + img_len);
  in.memory_start = in.initial_memory.empty() ? 0 : start;
  in.code.assign(code_tail.begin(),
                 code_tail.begin() + words * kInstructionBytes);
  return in;
}

// SplitMix64 -- the standard 64-bit mix used to expand one seed into an
// arbitrary-length deterministic schedule. Local copy: the harness must not
// depend on production code for its own randomness.
inline uint64_t
splitmix64(uint64_t x)
{
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e9b5ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

inline bool
ecall_args_equal(const EcallArgs &a, const EcallArgs &b)
{
  return a.a0 == b.a0 && a.a1 == b.a1 && a.a2 == b.a2 && a.a3 == b.a3 &&
         a.a4 == b.a4 && a.a5 == b.a5 && a.a6 == b.a6 && a.a7 == b.a7;
}

// Deterministic host schedule derived from a fuzzer-carved seed. Call i's
// result is a pure function of (seed, i) -- never of machine state -- so two
// machines given hosts with the same seed observe identical host behavior,
// which is the requirement for an engine differential run. The schedule
// exercises the host boundary the constant RecordingHost cannot: nonzero
// continue values landing in a0, host gas debits (including occasional
// over-budget ones that trip the engines' host-OOG debit branch), halts with
// a code, host out-of-gas results, and guest-memory writes through the
// MachineView (out-of-range addresses exercise the view's bounds rejection).
// HostStatus::kTrap is deliberately never produced: the threaded ecall seam
// forbids it by contract (ThreadedMachine::apply_ecall aborts).
class ScriptedHost final : public Host {
public:
  explicit ScriptedHost(uint64_t seed) : seed_(seed) {}

  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    // Harness bookkeeping only; an allocation failure recording test-only
    // state should terminate instead of escaping the noexcept host boundary.
    try {
      calls_.push_back(args);
    } catch (...) {
      std::terminate();
    }
    const uint64_t bits = splitmix64(seed_ + calls_.size());

    // A 4-byte schedule-derived write; addresses range past kFuzzMemCap so
    // small memories see identical in/out-of-bounds decisions in both engines.
    const auto addr = static_cast<uint32_t>((bits >> 32) % (kFuzzMemCap + 16));
    const std::array<uint8_t, 4> data = {static_cast<uint8_t>(bits),
                                         static_cast<uint8_t>(bits >> 8),
                                         static_cast<uint8_t>(bits >> 16),
                                         static_cast<uint8_t>(bits >> 24)};
    (void)view.write_memory(addr, data);

    // Mostly small debits; 1-in-16 exceeds kFuzzGasCap outright so the
    // engines' host-gas exhaustion branch is reachable at any carved budget.
    const uint64_t gas = ((bits >> 8) % 16 == 0)
                             ? kFuzzGasCap + ((bits >> 16) % 64)
                             : (bits >> 16) % 64;
    const auto value = static_cast<uint32_t>(bits >> 24);
    switch (bits % 8) {
    case 6:
      return HostResult::halt(value, gas);
    case 7:
      return HostResult::out_of_gas(gas);
    default:
      return HostResult::continue_with(value, gas);
    }
  }

  [[nodiscard]] const std::vector<EcallArgs> &calls() const { return calls_; }

private:
  uint64_t seed_ = 0;
  std::vector<EcallArgs> calls_;
};

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
