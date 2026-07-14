#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"

// Differential-testing substrate for the portable RV32IM core. Everything here
// depends only on rv32_isa_core (machine/program/host) -- no artifact,
// consensus, or syscall layer -- so the same primitives drive the in-tree
// spec-model crosscheck today and an external reference oracle (Sail/Spike)
// later. There is no production code here; it is harness infrastructure.

namespace hogd::rv32 {

// Canonical observable architectural state of the core at one instant. This is
// the unit of comparison for differential testing: any model (our Machine, a
// hand-written spec, or an external oracle) snapshots into this so two runs can
// be diffed through one representation.
//
// pc/regs/memory are pure-ISA architectural state, directly comparable against
// any RISC-V reference model. gas_remaining/trap/halted/halt_code are
// hogd-specific VM bookkeeping -- meaningful for Machine-vs-spec checks but not
// for an external oracle that models neither gas nor our trap taxonomy. A
// cross-oracle harness should compare the architectural subset (pc, regs,
// memory) and ignore the bookkeeping fields.
struct ArchState {
  uint32_t pc = 0;
  std::array<uint32_t, kRegisterCount> regs = {};
  std::vector<uint8_t> memory;
  uint64_t gas_remaining = 0;
  Trap trap = Trap::kNone;
  bool halted = false;
  uint32_t halt_code = 0;

  bool operator==(const ArchState &) const = default;
};

inline const char *
trap_name(Trap trap)
{
  switch (trap) {
  case Trap::kNone:
    return "none";
  case Trap::kInvalidInstruction:
    return "invalid_instruction";
  case Trap::kBadPc:
    return "bad_pc";
  case Trap::kBadMemory:
    return "bad_memory";
  case Trap::kBreakpoint:
    return "breakpoint";
  case Trap::kOutOfGas:
    return "out_of_gas";
  case Trap::kHostTrap:
    return "host_trap";
  }
  return "unknown";
}

// Snapshot the machine's full observable state via its public accessors only.
inline ArchState
capture_arch_state(const Machine &machine)
{
  ArchState state;
  state.pc = machine.pc();
  for (size_t i = 0; i < state.regs.size(); ++i) {
    state.regs[i] = machine.reg(i);
  }
  const std::span<const uint8_t> memory = machine.memory();
  state.memory.assign(memory.begin(), memory.end());
  state.gas_remaining = machine.gas_remaining();
  state.trap = machine.trap();
  state.halted = machine.halted();
  state.halt_code = machine.halt_code();
  return state;
}

// Empty string if the states are identical, otherwise a description of the
// FIRST differing field. Ordering reports the most localizing difference first
// (pc, then a specific register, then a specific memory byte) -- the "where did
// the two models first disagree" that differential testing exists to answer.
inline std::string
describe_divergence(const ArchState &expected, const ArchState &actual)
{
  if (expected.pc != actual.pc) {
    return std::format("pc: expected 0x{:08x}, actual 0x{:08x}", expected.pc,
                       actual.pc);
  }
  for (size_t i = 0; i < expected.regs.size(); ++i) {
    if (expected.regs[i] != actual.regs[i]) {
      return std::format("x{}: expected 0x{:08x}, actual 0x{:08x}", i,
                         expected.regs[i], actual.regs[i]);
    }
  }
  if (expected.memory.size() != actual.memory.size()) {
    return std::format("memory size: expected {}, actual {}",
                       expected.memory.size(), actual.memory.size());
  }
  for (size_t i = 0; i < expected.memory.size(); ++i) {
    if (expected.memory[i] != actual.memory[i]) {
      return std::format("memory[{}]: expected 0x{:02x}, actual 0x{:02x}", i,
                         expected.memory[i], actual.memory[i]);
    }
  }
  if (expected.halted != actual.halted) {
    return std::format("halted: expected {}, actual {}", expected.halted,
                       actual.halted);
  }
  if (expected.halt_code != actual.halt_code) {
    return std::format("halt_code: expected {}, actual {}", expected.halt_code,
                       actual.halt_code);
  }
  if (expected.trap != actual.trap) {
    return std::format("trap: expected {}, actual {}", trap_name(expected.trap),
                       trap_name(actual.trap));
  }
  if (expected.gas_remaining != actual.gas_remaining) {
    return std::format("gas_remaining: expected {}, actual {}",
                       expected.gas_remaining, actual.gas_remaining);
  }
  return "";
}

// A Host that executes no syscall: it records each ecall's register arguments
// and returns a fixed, caller-chosen result. This lets the pure ISA core run
// ecall-bearing programs in a differential harness with zero dependency on the
// hogd syscall layer (rv32_vm / syscall_bridge). The default halts the machine
// with a0, matching the minimal convention the in-tree crosscheck uses to
// terminate a program at its first ecall.
class RecordingHost final : public Host {
public:
  enum class OnEcall { kHaltWithA0, kContinue };

  explicit RecordingHost(OnEcall behaviour = OnEcall::kHaltWithA0)
    : behaviour_(behaviour)
  {}

  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    // Harness bookkeeping only. The Host boundary is noexcept; an allocation
    // failure while recording test-only state should terminate instead of
    // escaping through the VM host boundary.
    try {
      calls_.push_back(args);
    } catch (...) {
      std::terminate();
    }
    if (behaviour_ == OnEcall::kContinue) {
      return HostResult::continue_with(0, 0);
    }
    return HostResult::halt(view.reg(10));
  }

  [[nodiscard]] const std::vector<EcallArgs> &calls() const { return calls_; }
  [[nodiscard]] size_t call_count() const { return calls_.size(); }

private:
  OnEcall behaviour_;
  std::vector<EcallArgs> calls_;
};

struct TraceStep {
  StepResult result = StepResult::kContinue;
  ArchState state_before; // Architectural state BEFORE this instruction.
  ArchState state;        // Architectural state AFTER this instruction retired.
};

struct ExecTrace {
  std::vector<TraceStep> steps;
  StepResult final_result = StepResult::kTrap;
};

// Observer contract for differential tracing. A machine that runs to completion
// (e.g. a tail-call-threaded interpreter) templates its run loop on an observer
// and, after each retired instruction, calls on_retire. Instantiated with
// NullObserver the calls compile to nothing (kEnabled == false guards them via
// `if constexpr`), so the production path pays zero cost; instantiated with
// TraceObserver it accumulates the same ExecTrace that trace_execution
// produces, so describe_trace_divergence compares any machine uniformly.
struct NullObserver {
  static constexpr bool kEnabled = false;
  void on_initial(const ArchState & /*state*/) {}
  void on_retire(const ArchState & /*state*/, StepResult /*result*/) {}
};

class TraceObserver {
public:
  static constexpr bool kEnabled = true;

  // Architectural state before the first instruction -> step[0].state_before.
  void on_initial(const ArchState &state)
  {
    pending_before_ = state;
    initialized_ = true;
  }

  // Architectural state after a retired instruction. Chains so that
  // step[i].state_before == step[i-1].state, matching trace_execution. The
  // caller must resolve any ecall before calling this and pass the resolved
  // result; StepResult::kEcall must never be passed (it would be recorded as a
  // terminal final_result), exactly as trace_execution never stores kEcall.
  void on_retire(const ArchState &state, StepResult result)
  {
    // on_initial() must run first, or state_before would be a zeroed ArchState
    // for step[0] (silently wrong). The production run() always calls it; this
    // guards a future caller that wires the observer up out of order.
    assert(initialized_ && "on_initial() must be called before on_retire()");
    trace_.steps.push_back(TraceStep{.result = result,
                                     .state_before = pending_before_,
                                     .state = state});
    if (result != StepResult::kContinue) {
      trace_.final_result = result;
    }
    pending_before_ = state;
  }

  [[nodiscard]] const ExecTrace &trace() const { return trace_; }

private:
  ArchState pending_before_;
  bool initialized_ = false; // set by on_initial(); guards on_retire() ordering
  ExecTrace
      trace_; // final_result defaults to kTrap (truncation), as in ExecTrace.
};

// Run `machine` one instruction at a time -- dispatching any ecall through
// `host` -- capturing the full architectural state after each retired
// instruction. Stops at halt/trap, or after `max_steps` (recorded as a trailing
// kTrap to flag truncation, mirroring the runaway guard in the crosscheck). The
// per-step ArchState sequence is what a lockstep oracle comparison consumes.
inline ExecTrace
trace_execution(Machine &machine, Host &host, size_t max_steps = 4096)
{
  ExecTrace trace;
  for (size_t i = 0; i < max_steps; ++i) {
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
      return trace;
    }
  }
  trace.final_result = StepResult::kTrap; // Truncated at max_steps.
  return trace;
}

// Zero per-step gas_remaining so block-metered threaded traces compare against
// per-instruction canonical traces. Final gas is checked separately by callers.
inline ExecTrace
strip_trace_gas(ExecTrace trace)
{
  for (TraceStep &step : trace.steps) {
    step.state.gas_remaining = 0;
    step.state_before.gas_remaining = 0;
  }
  return trace;
}

// Empty string if the two traces match step for step, otherwise the index of
// the first diverging step and the field that differs -- the primary output a
// Machine-vs-oracle differential run reports.
inline std::string
describe_trace_divergence(const ExecTrace &expected, const ExecTrace &actual)
{
  const size_t common = std::min(expected.steps.size(), actual.steps.size());
  for (size_t i = 0; i < common; ++i) {
    const std::string diff =
        describe_divergence(expected.steps[i].state, actual.steps[i].state);
    if (!diff.empty()) {
      return std::format("step {}: {}", i, diff);
    }
    if (expected.steps[i].result != actual.steps[i].result) {
      return std::format("step {}: result expected {}, actual {}", i,
                         static_cast<int>(expected.steps[i].result),
                         static_cast<int>(actual.steps[i].result));
    }
  }
  if (expected.steps.size() != actual.steps.size()) {
    return std::format("step count: expected {}, actual {}",
                       expected.steps.size(), actual.steps.size());
  }
  if (expected.final_result != actual.final_result) {
    return std::format("final result expected {}, actual {}",
                       static_cast<int>(expected.final_result),
                       static_cast<int>(actual.final_result));
  }
  return "";
}

} // namespace hogd::rv32
