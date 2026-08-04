#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/arch_trace.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/oracle.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_corpus.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"

// Exercises the external-oracle differential harness (oracle.h) against the
// portable core. Linked against rv32_isa_core ONLY (see CMakeLists.txt): the
// harness must stay hogd-free so the same interface can later back an external
// reference model (Spike/sail-riscv). MachineOracle is the model under test;
// SpecModelOracle stands in for the external oracle here.

namespace hogd::rv32 {
namespace {

using test_program::addi;
using test_program::branch;
using test_program::load;
using test_program::op;
using test_program::store;

constexpr uint32_t kEcall = 0x00000073U;
constexpr uint8_t kAddFunct7 = 0x00;
constexpr uint8_t kSubFunct7 = 0x20;
constexpr uint8_t kMulDivFunct7 = 0x01;

DifferentialProgram
diff_program(std::vector<uint32_t> words, uint32_t memory_bytes = 256,
             uint64_t gas = 1000)
{
  DifferentialProgram program;
  program.code = std::move(words);
  program.memory_bytes = memory_bytes;
  program.initial_memory.assign(memory_bytes, 0);
  program.gas_limit = gas;
  return program;
}

TEST(Rv32Oracle, machineOracleExecutesWordsThroughInterface)
{
  MachineOracle machine_oracle;
  const ExecTrace trace =
      machine_oracle.run(diff_program({addi(1, 0, 5), addi(2, 1, 7), kEcall}));

  EXPECT_EQ(trace.final_result, StepResult::kHalted);
  ASSERT_EQ(trace.steps.size(), 3U);
  EXPECT_EQ(trace.steps[0].state.regs[1], 5U);
  EXPECT_EQ(trace.steps[1].state.regs[2], 12U); // 5 + 7
  EXPECT_TRUE(trace.steps.back().state.halted);
}

TEST(Rv32Oracle, specModelOracleMatchesMachineAcrossInstructionClasses)
{
  MachineOracle machine_oracle;
  SpecModelOracle spec_oracle;

  const std::vector<std::pair<std::string, std::vector<uint32_t>>> programs = {
      {"arithmetic",
       {addi(1, 0, 10), addi(2, 0, 3), op(3, 1, 2, 0, kAddFunct7),
        op(4, 1, 2, 0, kSubFunct7), kEcall}},
      {"muldiv",
       {addi(1, 0, 20), addi(2, 0, 6), op(3, 1, 2, 0, kMulDivFunct7),
        op(4, 1, 2, 4, kMulDivFunct7), op(5, 1, 2, 6, kMulDivFunct7), kEcall}},
      {"loadstore",
       {addi(1, 0, 16), addi(2, 0, 0x55), store(1, 2, 0, 2), load(3, 2, 1, 0),
        kEcall}},
      {"branch_taken",
       {addi(1, 0, 5), addi(2, 0, 5), branch(0, 1, 2, 8), addi(3, 0, 99),
        addi(4, 0, 77), kEcall}},
  };

  for (const auto &[name, words] : programs) {
    SCOPED_TRACE(name);
    const DifferentialProgram program = diff_program(words);
    const ExecTrace expected = spec_oracle.run(program);
    const ExecTrace actual = machine_oracle.run(program);
    EXPECT_EQ(actual.final_result, StepResult::kHalted);
    EXPECT_EQ(describe_arch_trace_divergence(expected, actual), "");
  }
}

TEST(Rv32Oracle, archDivergenceIgnoresBookkeepingFields)
{
  ArchState reference;
  reference.pc = 8;
  reference.regs[1] = 42;
  reference.memory = {0x01, 0x02, 0x03};

  // Same architectural subset, every hogd bookkeeping field perturbed.
  ArchState bookkeeping_only = reference;
  bookkeeping_only.gas_remaining = reference.gas_remaining + 99;
  bookkeeping_only.trap = Trap::kOutOfGas;
  bookkeeping_only.halted = !reference.halted;
  bookkeeping_only.halt_code = 7;
  EXPECT_EQ(describe_arch_divergence(reference, bookkeeping_only), "");

  ArchState reg_diff = reference;
  reg_diff.regs[1] = 43;
  EXPECT_NE(describe_arch_divergence(reference, reg_diff).find("x1"),
            std::string::npos);

  ArchState mem_diff = reference;
  mem_diff.memory[2] = 0xff;
  EXPECT_NE(describe_arch_divergence(reference, mem_diff).find("memory[2]"),
            std::string::npos);
}

TEST(Rv32Oracle, gasDifferenceDivergesOnlyUnderFullComparison)
{
  MachineOracle machine_oracle;
  const std::vector<uint32_t> words = {addi(1, 0, 5), addi(2, 1, 7), kEcall};

  const ExecTrace lean = machine_oracle.run(diff_program(words, 256, 50));
  const ExecTrace rich = machine_oracle.run(diff_program(words, 256, 500));

  // Identical architectural trajectory under the boundary contract ...
  EXPECT_EQ(describe_arch_trace_divergence(lean, rich), "");
  // ... while the full comparison (arch_trace.h) sees the gas_remaining gap.
  EXPECT_NE(describe_trace_divergence(lean, rich), "");
}

TEST(Rv32Oracle, contractStopsAtMutualTrapWithoutAllowingSideEffects)
{
  MachineOracle machine_oracle;
  SpecModelOracle spec_oracle;
  // Store at address == memory_bytes (256): out of bounds for a 4-byte store.
  const DifferentialProgram program =
      diff_program({addi(1, 0, 256), addi(2, 0, 1), store(1, 2, 0, 2), kEcall});

  const ExecTrace machine_trace = machine_oracle.run(program);
  const ExecTrace spec_trace = spec_oracle.run(program);

  ASSERT_EQ(machine_trace.final_result, StepResult::kTrap);
  ASSERT_EQ(spec_trace.final_result, StepResult::kTrap);
  EXPECT_EQ(describe_arch_trace_divergence(spec_trace, machine_trace), "");

  // A von-Neumann oracle would trap-and-vector to a different post-trap PC.
  // The contract tolerates that PC difference, but still rejects register or
  // memory side effects from the faulting instruction.
  ASSERT_FALSE(machine_trace.steps.empty());
  ExecTrace vectored = machine_trace;
  TraceStep &trap_step = vectored.steps.back();
  ASSERT_EQ(trap_step.result, StepResult::kTrap);
  trap_step.state.pc = 0x000dead0U;
  EXPECT_EQ(describe_arch_trace_divergence(machine_trace, vectored), "");

  ExecTrace side_effect = machine_trace;
  side_effect.steps.back().state.regs[5] = 0x12345678U;
  const std::string diff =
      describe_arch_trace_divergence(machine_trace, side_effect);
  EXPECT_NE(diff.find("trap side effect"), std::string::npos) << diff;
  EXPECT_NE(diff.find("x5"), std::string::npos) << diff;
}

TEST(Rv32Oracle, contractLocalizesComputationalDivergence)
{
  MachineOracle machine_oracle;
  const ExecTrace add_trace = machine_oracle.run(diff_program(
      {addi(1, 0, 10), addi(2, 0, 3), op(3, 1, 2, 0, kAddFunct7), kEcall}));
  const ExecTrace sub_trace = machine_oracle.run(diff_program(
      {addi(1, 0, 10), addi(2, 0, 3), op(3, 1, 2, 0, kSubFunct7), kEcall}));

  const std::string diff = describe_arch_trace_divergence(add_trace, sub_trace);
  EXPECT_NE(diff.find("step 2"), std::string::npos) << diff;
  EXPECT_NE(diff.find("x3"), std::string::npos) << diff; // 13 vs 7
}

TEST(Rv32Oracle, contractComparesFinalResultForEmptyTraces)
{
  ExecTrace reference{.steps = {}, .final_result = StepResult::kTrap};
  ExecTrace actual{.steps = {}, .final_result = StepResult::kHalted};

  const std::string diff = describe_arch_trace_divergence(reference, actual);
  EXPECT_NE(diff.find("final result"), std::string::npos) << diff;
}

TEST(Rv32Oracle, invalidDifferentialInputsRejectedConsistently)
{
  MachineOracle machine_oracle;
  SpecModelOracle spec_oracle;

  DifferentialProgram oversized_memory = diff_program({kEcall}, 4);
  oversized_memory.initial_memory.assign(5, 0xaa);
  ASSERT_FALSE(valid_differential_program(oversized_memory));

  const ExecTrace machine_memory_trace = machine_oracle.run(oversized_memory);
  const ExecTrace spec_memory_trace = spec_oracle.run(oversized_memory);
  EXPECT_EQ(machine_memory_trace.final_result, StepResult::kTrap);
  EXPECT_EQ(spec_memory_trace.final_result, StepResult::kTrap);
  EXPECT_TRUE(machine_memory_trace.steps.empty());
  EXPECT_TRUE(spec_memory_trace.steps.empty());
  EXPECT_EQ(describe_arch_trace_divergence(spec_memory_trace,
                                           machine_memory_trace),
            "");

  DifferentialProgram excessive_memory_bytes = diff_program({kEcall});
  excessive_memory_bytes.memory_bytes = kMaxMemoryBytes + 1;
  ASSERT_FALSE(valid_differential_program(excessive_memory_bytes));
  EXPECT_TRUE(machine_oracle.run(excessive_memory_bytes).steps.empty());
  EXPECT_TRUE(spec_oracle.run(excessive_memory_bytes).steps.empty());
}

TEST(Rv32Oracle, invalidProgramRejectedConsistently)
{
  MachineOracle machine_oracle;
  SpecModelOracle spec_oracle;
  // 0xffffffff is not a valid RV32IM encoding: rejected before any step runs.
  const DifferentialProgram program = diff_program({0xffffffffU});

  const ExecTrace machine_trace = machine_oracle.run(program);
  const ExecTrace spec_trace = spec_oracle.run(program);

  EXPECT_EQ(machine_trace.final_result, StepResult::kTrap);
  EXPECT_EQ(spec_trace.final_result, StepResult::kTrap);
  EXPECT_TRUE(machine_trace.steps.empty());
  EXPECT_TRUE(spec_trace.steps.empty());
  EXPECT_EQ(describe_arch_trace_divergence(spec_trace, machine_trace), "");
}

TEST(Rv32Oracle, runningOffCodeImageTrapsConsistently)
{
  MachineOracle machine_oracle;
  SpecModelOracle spec_oracle;
  // No terminating ecall: PC walks off the end of the code image (kBadPc).
  const DifferentialProgram program =
      diff_program({addi(1, 0, 1), addi(2, 0, 2)});

  const ExecTrace machine_trace = machine_oracle.run(program);
  const ExecTrace spec_trace = spec_oracle.run(program);

  EXPECT_EQ(machine_trace.final_result, StepResult::kTrap);
  EXPECT_EQ(spec_trace.final_result, StepResult::kTrap);
  EXPECT_EQ(describe_arch_trace_divergence(spec_trace, machine_trace), "");
}

// Fast, non-gated breadth: the constrained-random register-only corpus
// (test_corpus.h) -- the same corpus the gated Spike test uses -- run through
// Machine and the spec model. Validates the generator in default CI and makes a
// Machine-vs-Spike failure attributable (if Machine and spec already agree
// here, a Spike divergence points at the Spike mapping, not the VM).
TEST(Rv32Oracle, machineMatchesSpecOnRandomRegisterCorpus)
{
  MachineOracle machine_oracle;
  SpecModelOracle spec_oracle;
  for (uint64_t seed = 0; seed < 256; ++seed) {
    SCOPED_TRACE(seed);
    DifferentialProgram program;
    program.code =
        test_corpus::generate_register_only_program(0xC0FFEEULL + seed, 64);
    program.memory_bytes = 256;
    program.initial_memory.assign(256, 0);
    const ExecTrace expected = spec_oracle.run(program);
    const ExecTrace actual = machine_oracle.run(program);
    EXPECT_EQ(actual.final_result, StepResult::kHalted);
    EXPECT_EQ(describe_arch_trace_divergence(expected, actual), "");
  }
}

// Same as above for the memory corpus (loads/stores) -- validates the memory
// generator and the load/store data path Machine-vs-spec in default CI.
TEST(Rv32Oracle, machineMatchesSpecOnRandomMemoryCorpus)
{
  MachineOracle machine_oracle;
  SpecModelOracle spec_oracle;
  for (uint64_t seed = 0; seed < 256; ++seed) {
    SCOPED_TRACE(seed);
    DifferentialProgram program;
    program.code = test_corpus::generate_memory_program(0xBEEFULL + seed, 64);
    program.memory_bytes = test_corpus::kCorpusMemoryBytes;
    program.initial_memory.assign(test_corpus::kCorpusMemoryBytes, 0);
    const ExecTrace expected = spec_oracle.run(program);
    const ExecTrace actual = machine_oracle.run(program);
    EXPECT_EQ(actual.final_result, StepResult::kHalted);
    EXPECT_EQ(describe_arch_trace_divergence(expected, actual), "");
  }
}

} // namespace
} // namespace hogd::rv32
