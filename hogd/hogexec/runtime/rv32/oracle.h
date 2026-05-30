#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/spec_model.h"
#include "hogd/hogexec/runtime/rv32/test_support.h"

// External-oracle differential-testing harness for the portable RV32IM core.
// An Oracle is any independent model that, given a model-neutral program +
// initial architectural state, produces the per-step ArchState trace
// (arch_trace.h). MachineOracle wraps our production Machine; SpecModelOracle
// wraps the in-tree hand spec model (spec_model.h); a future SpikeOracle /
// SailOracle adapter slots in behind the same interface. The boundary-contract
// comparison below is what every external-oracle run uses: it compares only the
// architectural subset (pc/regs/memory), excludes hogd-only bookkeeping (gas,
// trap taxonomy), and stops at the first trap/halt. On mutual traps it compares
// register/memory side effects only and allows pc to diverge because a real
// RISC-V model vectors while this profile halts. There is no production code
// here; it is harness infrastructure.

namespace hogd::rv32 {

// Model-neutral description of one differential test program: the shared input
// every oracle consumes so two independent implementations execute the SAME
// program from the SAME initial architectural state.
//
// `code` is raw little-endian instruction words -- the lowest common
// denominator across models, since an external oracle decodes bytes
// independently rather than sharing our DecodedInstruction. `initial_memory` is
// written at data address 0 over a zero-filled region of `memory_bytes`.
// `initial_memory` must fit in `memory_bytes`. `gas_limit` is hogd-only; normal
// differential runs should keep it non-binding so an external oracle that
// models no gas can still compare against the same execution boundary.
struct DifferentialProgram {
  std::vector<uint32_t> code;
  std::array<uint32_t, kRegisterCount> initial_regs = {};
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = 0;
  uint32_t initial_pc = 0;
  uint64_t gas_limit = std::numeric_limits<uint64_t>::max();
};

class Oracle {
public:
  Oracle() = default;
  Oracle(const Oracle &) = default;
  Oracle(Oracle &&) = default;
  Oracle &operator=(const Oracle &) = default;
  Oracle &operator=(Oracle &&) = default;
  virtual ~Oracle() = default;

  [[nodiscard]] virtual ExecTrace run(const DifferentialProgram &program) = 0;
};

inline std::vector<uint8_t>
code_bytes_from_words(std::span<const uint32_t> words)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(words.size() * kInstructionBytes);
  for (const uint32_t word : words) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
  }
  return bytes;
}

inline bool
valid_differential_program(const DifferentialProgram &program)
{
  return program.code.size() <= (kMaxCodeBytes / kInstructionBytes) &&
         program.memory_bytes <= kMaxMemoryBytes &&
         program.initial_memory.size() <= program.memory_bytes;
}

inline ExecTrace
invalid_differential_program_trace()
{
  return ExecTrace{.steps = {}, .final_result = StepResult::kTrap};
}

// Our production Machine behind the Oracle interface: build an immutable
// Program from the raw words, apply the initial register/memory state, and
// trace execution. The ecall-free corpus never expects the host to fire; if it
// does, RecordingHost's default halts so the trace terminates
// deterministically.
class MachineOracle final : public Oracle {
public:
  [[nodiscard]] ExecTrace run(const DifferentialProgram &program) override
  {
    if (!valid_differential_program(program)) {
      return invalid_differential_program_trace();
    }
    const std::vector<uint8_t> bytes = code_bytes_from_words(program.code);
    auto created =
        Program::create(std::span<const uint8_t>(bytes.data(), bytes.size()));
    if (!created.has_value()) {
      // A program our validator rejects never executes. Surface as an immediate
      // trap with no steps so it compares equal to any other model that also
      // rejects it.
      return invalid_differential_program_trace();
    }
    auto image = std::make_shared<Program>(std::move(*created));
    Machine machine(image, program.memory_bytes, program.gas_limit,
                    program.initial_pc);
    for (size_t i = 1; i < program.initial_regs.size(); ++i) {
      MachineTestPeer::set_reg(machine, i, program.initial_regs[i]);
    }
    if (!program.initial_memory.empty()) {
      const bool wrote = machine.view().write_memory(0, program.initial_memory);
      if (!wrote) {
        return invalid_differential_program_trace();
      }
    }
    RecordingHost host;
    return trace_execution(machine, host);
  }
};

// The in-tree hand spec model behind the Oracle interface: the first
// independent producer, standing in for an external oracle so the harness shape
// is exercised with zero external dependency.
class SpecModelOracle final : public Oracle {
public:
  [[nodiscard]] ExecTrace run(const DifferentialProgram &program) override
  {
    if (!valid_differential_program(program)) {
      return invalid_differential_program_trace();
    }
    std::vector<spec::SpecInstruction> instructions;
    instructions.reserve(program.code.size());
    for (const uint32_t word : program.code) {
      const DecodeResult decoded = decode_instruction(word);
      if (!decoded.ok) {
        return invalid_differential_program_trace();
      }
      instructions.push_back(spec::decoded_to_spec(decoded.instruction));
    }

    spec::SpecState state;
    state.regs = program.initial_regs;
    state.regs[0] = 0;
    state.memory.assign(program.memory_bytes, 0);
    std::copy_n(program.initial_memory.begin(), program.initial_memory.size(),
                state.memory.begin());
    state.pc = program.initial_pc;
    state.gas_remaining = program.gas_limit;

    return spec::trace_spec(state,
                      std::span<const spec::SpecInstruction>(instructions.data(),
                                                       instructions.size()));
  }
};

// Architectural-subset divergence: compares ONLY pc, regs, and memory -- the
// fields any RISC-V model shares. Deliberately excludes gas_remaining and the
// hogd trap/halt taxonomy, which an external oracle does not model. Empty
// string if the architectural state matches.
inline std::string
describe_arch_divergence(const ArchState &reference, const ArchState &actual)
{
  if (reference.pc != actual.pc) {
    return std::format("pc: reference 0x{:08x}, actual 0x{:08x}", reference.pc,
                       actual.pc);
  }
  for (size_t i = 0; i < reference.regs.size(); ++i) {
    if (reference.regs[i] != actual.regs[i]) {
      return std::format("x{}: reference 0x{:08x}, actual 0x{:08x}", i,
                         reference.regs[i], actual.regs[i]);
    }
  }
  if (reference.memory.size() != actual.memory.size()) {
    return std::format("memory size: reference {}, actual {}",
                       reference.memory.size(), actual.memory.size());
  }
  for (size_t i = 0; i < reference.memory.size(); ++i) {
    if (reference.memory[i] != actual.memory[i]) {
      return std::format("memory[{}]: reference 0x{:02x}, actual 0x{:02x}", i,
                         reference.memory[i], actual.memory[i]);
    }
  }
  return "";
}

inline std::string
describe_reg_memory_divergence(const ArchState &reference,
                               const ArchState &actual)
{
  for (size_t i = 0; i < reference.regs.size(); ++i) {
    if (reference.regs[i] != actual.regs[i]) {
      return std::format("x{}: reference 0x{:08x}, actual 0x{:08x}", i,
                         reference.regs[i], actual.regs[i]);
    }
  }
  if (reference.memory.size() != actual.memory.size()) {
    return std::format("memory size: reference {}, actual {}",
                       reference.memory.size(), actual.memory.size());
  }
  for (size_t i = 0; i < reference.memory.size(); ++i) {
    if (reference.memory[i] != actual.memory[i]) {
      return std::format("memory[{}]: reference 0x{:02x}, actual 0x{:02x}", i,
                         reference.memory[i], actual.memory[i]);
    }
  }
  return "";
}

inline const char *
result_category(StepResult result)
{
  switch (result) {
  case StepResult::kContinue:
    return "continue";
  case StepResult::kEcall:
    return "ecall";
  case StepResult::kHalted:
    return "halt";
  case StepResult::kTrap:
    return "trap";
  }
  return "unknown";
}

// Compare two traces under the cross-oracle boundary contract:
//   - architectural subset only (pc/regs/memory; gas and trap taxonomy
//     excluded), so a model that meters no gas still compares equal;
//   - the comparison terminates at the first step where either model stops
//     advancing. Both halting compares the (well-defined) halt state. Both
//     trapping agrees a fault occurred at that step; register/memory side
//     effects on the faulting instruction are rejected, but pc may diverge
//     because we halt there while a real RISC-V model vectors via mepc/mcause.
// Empty string if the architectural trajectories agree up to that boundary.
inline std::string
describe_arch_trace_divergence(const ExecTrace &reference,
                               const ExecTrace &actual)
{
  const size_t common = std::min(reference.steps.size(), actual.steps.size());
  for (size_t i = 0; i < common; ++i) {
    const TraceStep &ref_step = reference.steps[i];
    const TraceStep &act_step = actual.steps[i];
    const bool ref_running = ref_step.result == StepResult::kContinue;
    const bool act_running = act_step.result == StepResult::kContinue;

    if (ref_running && act_running) {
      const std::string diff =
          describe_arch_divergence(ref_step.state, act_step.state);
      if (!diff.empty()) {
        return std::format("step {}: {}", i, diff);
      }
      continue;
    }

    // At least one model terminates at this step -- the boundary.
    if (ref_running != act_running) {
      return std::format(
          "step {}: trajectory length diverged (reference {}, actual {})", i,
          result_category(ref_step.result), result_category(act_step.result));
    }

    const bool both_halt = ref_step.result == StepResult::kHalted &&
                           act_step.result == StepResult::kHalted;
    const bool both_trap = ref_step.result == StepResult::kTrap &&
                           act_step.result == StepResult::kTrap;
    if (!both_halt && !both_trap) {
      return std::format(
          "step {}: termination diverged (reference {}, actual {})", i,
          result_category(ref_step.result), result_category(act_step.result));
    }
    if (both_halt) {
      const std::string diff =
          describe_arch_divergence(ref_step.state, act_step.state);
      if (!diff.empty()) {
        return std::format("step {} (halt): {}", i, diff);
      }
    }
    if (both_trap) {
      const std::string pre_diff =
          describe_arch_divergence(ref_step.state_before,
                                   act_step.state_before);
      if (!pre_diff.empty()) {
        return std::format("step {} (trap pre-state): {}", i, pre_diff);
      }
      const std::string ref_effect =
          describe_reg_memory_divergence(ref_step.state_before, ref_step.state);
      if (!ref_effect.empty()) {
        return std::format("step {} (reference trap side effect): {}", i,
                           ref_effect);
      }
      const std::string act_effect =
          describe_reg_memory_divergence(act_step.state_before, act_step.state);
      if (!act_effect.empty()) {
        return std::format("step {} (actual trap side effect): {}", i,
                           act_effect);
      }
    }
    // both_halt matched, or both_trap matched without register/memory side
    // effects -- the models agree up to the boundary.
    return "";
  }

  // No boundary inside the common prefix: both still running there. A length
  // mismatch then means one ran longer (e.g. truncation) -- a real divergence.
  if (reference.steps.size() != actual.steps.size()) {
    return std::format("step count: reference {}, actual {}",
                       reference.steps.size(), actual.steps.size());
  }
  if (reference.final_result != actual.final_result) {
    return std::format("final result reference {}, actual {}",
                       result_category(reference.final_result),
                       result_category(actual.final_result));
  }
  return "";
}

} // namespace hogd::rv32
