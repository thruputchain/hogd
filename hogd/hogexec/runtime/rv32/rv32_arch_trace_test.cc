#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/test_corpus.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

// Exercises the differential-testing substrate (arch_trace.h) against the
// portable core. Linked against rv32_isa_core ONLY (see CMakeLists.txt), so it
// also reasserts that the substrate carries no hogd dependencies and can back
// an external oracle harness.

namespace hogd::rv32 {
namespace {

using test_program::addi;
using test_program::make_program;
using test_program::op;

constexpr uint32_t kEcall = 0x00000073U;

TEST(Rv32ArchTrace, captureReflectsMachineState)
{
  auto program = make_program({addi(1, 0, 5), addi(2, 1, 7)});
  ASSERT_NE(program, nullptr);
  Machine machine(program, /*memory_bytes=*/256, /*gas=*/100);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  ASSERT_EQ(machine.step(), StepResult::kContinue);

  const ArchState state = capture_arch_state(machine);
  EXPECT_EQ(state.pc, 8U);
  EXPECT_EQ(state.regs[1], 5U);
  EXPECT_EQ(state.regs[2], 12U);
  EXPECT_EQ(state.regs[0], 0U);
  EXPECT_EQ(state.gas_remaining, 98U);
  EXPECT_EQ(state.trap, Trap::kNone);
  EXPECT_FALSE(state.halted);
  EXPECT_EQ(state.memory.size(), 256U);
}

TEST(Rv32ArchTrace, describeDivergencePinpointsFirstDifferingField)
{
  ArchState base;
  base.pc = 4;
  base.regs[2] = 12;
  base.memory = {0x00, 0x11, 0x22};

  EXPECT_EQ(describe_divergence(base, base), "");

  ArchState reg_diff = base;
  reg_diff.regs[2] = 13;
  EXPECT_NE(describe_divergence(base, reg_diff).find("x2"), std::string::npos);

  ArchState pc_diff = base;
  pc_diff.pc = 8;
  EXPECT_NE(describe_divergence(base, pc_diff).find("pc"), std::string::npos);

  ArchState mem_diff = base;
  mem_diff.memory[1] = 0xff;
  EXPECT_NE(describe_divergence(base, mem_diff).find("memory[1]"),
            std::string::npos);

  ArchState memory_and_trap_diff = base;
  memory_and_trap_diff.memory[1] = 0xee;
  memory_and_trap_diff.trap = Trap::kBadMemory;
  EXPECT_NE(describe_divergence(base, memory_and_trap_diff).find("memory[1]"),
            std::string::npos);

  ArchState trap_diff = base;
  trap_diff.trap = Trap::kBadMemory;
  EXPECT_NE(describe_divergence(base, trap_diff).find("bad_memory"),
            std::string::npos);
}

TEST(Rv32ArchTrace, recordingHostCapturesEcallArgsWithoutSyscallLayer)
{
  // a0=7, a1=9, then ecall -- runs with no rv32_vm / syscall bridge present.
  auto program = make_program({addi(10, 0, 7), addi(11, 0, 9), kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, /*memory_bytes=*/256, /*gas=*/100);

  RecordingHost host;
  const ExecTrace trace = trace_execution(machine, host);

  EXPECT_EQ(trace.final_result, StepResult::kHalted);
  ASSERT_EQ(host.call_count(), 1U);
  EXPECT_EQ(host.calls()[0].a0, 7U);
  EXPECT_EQ(host.calls()[0].a1, 9U);
  ASSERT_FALSE(trace.steps.empty());
  EXPECT_TRUE(trace.steps.back().state.halted);
  EXPECT_EQ(trace.steps.back().state.halt_code, 7U); // halt-with-a0 default
}

TEST(Rv32ArchTrace, tracePerStepStatesMatchExecution)
{
  auto program = make_program({addi(1, 0, 5), addi(2, 1, 7), kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, /*memory_bytes=*/256, /*gas=*/100);

  RecordingHost host;
  const ExecTrace trace = trace_execution(machine, host);

  ASSERT_EQ(trace.steps.size(), 3U);
  EXPECT_EQ(trace.steps[0].result, StepResult::kContinue);
  EXPECT_EQ(trace.steps[0].state.regs[1], 5U);
  EXPECT_EQ(trace.steps[1].state.regs[2], 12U);
  EXPECT_EQ(trace.steps[2].result, StepResult::kHalted);
}

TEST(Rv32ArchTrace, lockstepTracesAgreeForIdenticalRuns)
{
  // Same program, two machines -> deterministic, identical traces. This is the
  // shape a Machine-vs-oracle comparison takes; here both sides are our core.
  auto program =
      make_program({addi(1, 0, 10), addi(2, 0, 3), op(3, 1, 2, 0, 0), kEcall});
  ASSERT_NE(program, nullptr);

  Machine lhs(program, /*memory_bytes=*/256, /*gas=*/100);
  Machine rhs(program, /*memory_bytes=*/256, /*gas=*/100);
  RecordingHost lhs_host;
  RecordingHost rhs_host;
  const ExecTrace lhs_trace = trace_execution(lhs, lhs_host);
  const ExecTrace rhs_trace = trace_execution(rhs, rhs_host);

  EXPECT_EQ(describe_trace_divergence(lhs_trace, rhs_trace), "");
  EXPECT_EQ(lhs_trace.steps[2].state.regs[3], 13U); // 10 + 3
}

TEST(Rv32ArchTrace, lockstepTraceLocatesComputationalDivergence)
{
  // Identical inputs, different op (ADD vs SUB) at step 2 -> the harness should
  // pinpoint the first diverging step and register. This is exactly what an
  // external-oracle mismatch would surface.
  auto add_program = make_program(
      {addi(1, 0, 10), addi(2, 0, 3), op(3, 1, 2, 0, 0x00), kEcall});
  auto sub_program = make_program(
      {addi(1, 0, 10), addi(2, 0, 3), op(3, 1, 2, 0, 0x20), kEcall});
  ASSERT_NE(add_program, nullptr);
  ASSERT_NE(sub_program, nullptr);

  Machine add_machine(add_program, /*memory_bytes=*/256, /*gas=*/100);
  Machine sub_machine(sub_program, /*memory_bytes=*/256, /*gas=*/100);
  RecordingHost add_host;
  RecordingHost sub_host;
  const ExecTrace add_trace = trace_execution(add_machine, add_host);
  const ExecTrace sub_trace = trace_execution(sub_machine, sub_host);

  const std::string diff = describe_trace_divergence(add_trace, sub_trace);
  EXPECT_NE(diff.find("step 2"), std::string::npos) << diff;
  EXPECT_NE(diff.find("x3"), std::string::npos) << diff;
  EXPECT_EQ(add_trace.steps[2].state.regs[3], 13U); // 10 + 3
  EXPECT_EQ(sub_trace.steps[2].state.regs[3], 7U);  // 10 - 3
}

TEST(Rv32ArchTrace, traceDivergenceComparesFinalResult)
{
  ArchState state;
  state.pc = 4;
  ExecTrace expected{
      .steps = {TraceStep{.result = StepResult::kContinue, .state = state}},
      .final_result = StepResult::kHalted,
  };
  ExecTrace actual = expected;
  actual.final_result = StepResult::kTrap;

  const std::string diff = describe_trace_divergence(expected, actual);
  EXPECT_NE(diff.find("final result"), std::string::npos) << diff;
}

TEST(Rv32ArchTrace, traceObserverChainsStatesIntoExecTrace)
{
  ArchState s0;
  s0.pc = 0;
  ArchState s1;
  s1.pc = 4;
  ArchState s2;
  s2.pc = 8;

  TraceObserver obs;
  obs.on_initial(s0);
  obs.on_retire(s1, StepResult::kContinue);
  obs.on_retire(s2, StepResult::kHalted);

  const ExecTrace &trace = obs.trace();
  ASSERT_EQ(trace.steps.size(), 2U);
  EXPECT_EQ(trace.steps[0].state_before.pc, 0U);
  EXPECT_EQ(trace.steps[0].state.pc, 4U);
  EXPECT_EQ(trace.steps[0].result, StepResult::kContinue);
  EXPECT_EQ(trace.steps[1].state_before.pc, 4U);
  EXPECT_EQ(trace.steps[1].state.pc, 8U);
  EXPECT_EQ(trace.steps[1].result, StepResult::kHalted);
  EXPECT_EQ(trace.final_result, StepResult::kHalted);
}

// Test-only adapter: drives the canonical Machine through the observer
// protocol, mimicking how a run-to-completion experimental machine would emit a
// trace.
inline ExecTrace
traced_run(Machine &machine, Host &host, size_t max_steps = 4096)
{
  TraceObserver obs;
  obs.on_initial(capture_arch_state(machine));
  for (size_t i = 0; i < max_steps; ++i) {
    StepResult result = machine.step();
    if (result == StepResult::kEcall) {
      const auto pending = machine.pending_ecall();
      result = !pending.has_value()
                   ? StepResult::kTrap
                   : machine.complete_ecall(
                         host.ecall(machine.view(), pending.value()));
    }
    obs.on_retire(capture_arch_state(machine), result);
    if (result != StepResult::kContinue) {
      break;
    }
  }
  return obs.trace();
}

TEST(Rv32ArchTrace, observerTraceMatchesStepTracerOverCorpus)
{
  // Register-only corpus programs exercise the full
  // arithmetic/logical/shift/mul/ div data path, forward branches, and the
  // ecall terminator -- enough to prove the observer path produces the
  // canonical trace. Memory load/store coverage is out of scope here (a
  // generate_memory_program variant exists for later).
  for (uint64_t seed = 0; seed < 32; ++seed) {
    const std::vector<uint32_t> code =
        test_corpus::generate_register_only_program(seed, /*body_len=*/64);
    const auto program =
        make_program(std::span<const uint32_t>(code.data(), code.size()));
    ASSERT_NE(program, nullptr) << "seed " << seed;

    Machine step_machine(program, /*memory_bytes=*/4096, /*gas=*/100000);
    RecordingHost step_host;
    const ExecTrace step_trace = trace_execution(step_machine, step_host);

    Machine obs_machine(program, /*memory_bytes=*/4096, /*gas=*/100000);
    RecordingHost obs_host;
    const ExecTrace obs_trace = traced_run(obs_machine, obs_host);

    EXPECT_EQ(describe_trace_divergence(step_trace, obs_trace), "")
        << "seed " << seed;

    // describe_trace_divergence intentionally compares state/result but NOT
    // state_before, so assert state_before parity explicitly here -- this is
    // the end-to-end check that the observer's chaining is correct over real
    // runs (the synthetic traceObserverChainsStatesIntoExecTrace covers the
    // protocol).
    ASSERT_EQ(obs_trace.steps.size(), step_trace.steps.size())
        << "seed " << seed;
    for (size_t i = 0; i < step_trace.steps.size(); ++i) {
      EXPECT_EQ(obs_trace.steps[i].state_before,
                step_trace.steps[i].state_before)
          << "seed " << seed << " step " << i;
    }
  }
}

} // namespace
} // namespace hogd::rv32
