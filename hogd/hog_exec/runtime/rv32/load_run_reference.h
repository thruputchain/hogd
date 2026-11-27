#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "hogd/hog_exec/runtime/rv32/arch_trace.h"
#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/machine.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

// Test support: canonical execution under the threaded engine's lw-run fault
// rule (op_load_run).
namespace hogd::rv32 {

// The trap op_load_run reports for a run that failed its range check: at the
// run's first slot, kBadMemory with the gas of a fault at that lw, or
// kOutOfGas when that lw itself is unaffordable.
struct LoadRunFault {
  uint32_t pc = 0;
  Trap trap = Trap::kNone;
  uint64_t gas_remaining = 0;
};

// Tracks which slots the engine dispatches: a passed run's remaining members
// do not apply their own runs; any other arrival at a run head does.
class LoadRunRule {
public:
  LoadRunRule(const ThreadedMachinePreInstantiation &lowered,
              std::span<const uint8_t> code)
    : lowered_(lowered), code_(code)
  {}

  // Call before executing each instruction.
  std::optional<LoadRunFault> before_step(const Machine &machine)
  {
    const uint32_t pc = machine.pc();
    const bool in_code = pc % kInstructionBytes == 0 &&
                         pc / kInstructionBytes < code_.size() / 4;
    const size_t index = pc / kInstructionBytes;
    // Covered: the next member of the run the engine is executing.
    if (in_code && index == run_next_ && index < run_end_) {
      ++run_next_;
      return std::nullopt;
    }
    run_next_ = run_end_ = 0;
    if (!in_code) {
      return std::nullopt;
    }
    const auto run = lowered_.load_run_at(index);
    if (!run.has_value()) {
      return std::nullopt;
    }
    const uint32_t word = load_u32_le(code_, size_t{pc});
    const uint32_t base = machine.reg((word >> 15) & 0x1fU);
    const auto imm = static_cast<uint32_t>(static_cast<int32_t>(word) >> 20);
    const uint32_t head = base + imm;
    const uint32_t lowest =
        run->descending ? head - ((run->length - 1) * 4) : head;
    if (uint64_t{lowest} + (uint64_t{run->length} * 4) <=
        machine.memory().size()) {
      run_next_ = index + 1;
      run_end_ = index + run->length;
      return std::nullopt;
    }
    const uint64_t cost = gas_cost_for_op(Op::kLw);
    const uint64_t gas = machine.gas_remaining();
    if (gas < cost) {
      return LoadRunFault{.pc = pc,
                          .trap = Trap::kOutOfGas,
                          .gas_remaining = 0};
    }
    return LoadRunFault{.pc = pc,
                        .trap = Trap::kBadMemory,
                        .gas_remaining = gas - cost};
  }

private:
  const ThreadedMachinePreInstantiation &lowered_;
  std::span<const uint8_t> code_;
  // The members [run_next_, run_end_) of the passed run still to execute.
  size_t run_next_ = 0;
  size_t run_end_ = 0;
};

// trace_execution (arch_trace.h) that stops where `lowered`'s lw-run rule
// faults. Returns that fault, or nullopt if the machine stopped (or the trace
// was truncated) without one; the trace holds the steps executed before it.
inline std::optional<LoadRunFault>
trace_execution_with_load_runs(Machine &machine, Host &host,
                               const ThreadedMachinePreInstantiation &lowered,
                               std::span<const uint8_t> code, ExecTrace &trace,
                               size_t max_steps = 4096)
{
  LoadRunRule rule(lowered, code);
  for (size_t i = 0; i < max_steps; ++i) {
    if (const auto fault = rule.before_step(machine)) {
      trace.final_result = StepResult::kTrap;
      return fault;
    }
    const ArchState state_before = capture_arch_state(machine);
    StepResult result = machine.step();
    if (result == StepResult::kEcall) {
      const auto pending = machine.pending_ecall();
      if (!pending.has_value()) {
        result = StepResult::kTrap;
      }
      else {
        result = machine.complete_ecall(host.ecall(machine.view(), *pending));
      }
    }
    trace.steps.push_back(TraceStep{.result = result,
                                    .state_before = state_before,
                                    .state = capture_arch_state(machine)});
    if (result != StepResult::kContinue) {
      trace.final_result = result;
      return std::nullopt;
    }
  }
  trace.final_result = StepResult::kTrap; // Truncated at max_steps.
  return std::nullopt;
}

} // namespace hogd::rv32
