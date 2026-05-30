#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/oracle.h"
#include "hogd/hogexec/runtime/rv32/spike_oracle.h"
#include "hogd/hogexec/runtime/rv32/test_corpus.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

// External-oracle differential test (C3): runs a corpus through both our
// in-tree oracles and the real Spike (riscv-isa-sim) reference simulator,
// asserting the architectural trajectories agree. Spike is located via
// RV32_ORACLE_BIN; the test SKIPS when it is unset/missing, so default builds
// and CI are unaffected (opt-in / out-of-default-CI posture). Linked against
// rv32_isa_core ONLY.
//
// First increment: register-only, ecall-terminated, zero-initial-register
// programs (no loads/stores, no jalr, no auipc / linking jal), so our 0-based
// pc-space maps to Spike's DRAM by a constant offset with no address-space
// mismatch. Memory programs and computed jumps are the next increments.

namespace hogd::rv32 {
namespace {

using test_program::addi;
using test_program::branch;
using test_program::j_type;
using test_program::op;

constexpr uint32_t kEcall = 0x00000073U;
constexpr uint8_t kF7Base = 0x00;   // add, sll, xor, srl, or, and
constexpr uint8_t kF7Alt = 0x20;    // sub, sra
constexpr uint8_t kF7MulDiv = 0x01; // mul, div, rem

DifferentialProgram
reg_program(std::vector<uint32_t> words)
{
  DifferentialProgram program;
  program.code = std::move(words);
  program.memory_bytes = 256;
  program.initial_memory.assign(256, 0);
  return program;
}

// Run the program through Spike once and assert both in-tree oracles agree with
// it on the architectural trajectory.
void
expect_oracles_agree_with_spike(SpikeOracle &spike, MachineOracle &machine,
                                SpecModelOracle &spec,
                                const DifferentialProgram &program)
{
  const ExecTrace spike_trace = spike.run(program);
  // More than the synthetic halt step -> Spike actually retired the program.
  EXPECT_GT(spike_trace.steps.size(), 1U) << "no Spike commits parsed";
  EXPECT_EQ(describe_arch_trace_divergence(machine.run(program), spike_trace),
            "")
      << "Machine vs Spike";
  EXPECT_EQ(describe_arch_trace_divergence(spec.run(program), spike_trace), "")
      << "spec model vs Spike";
}

struct NamedProgram {
  std::string name;
  std::vector<uint32_t> words;
};

std::vector<NamedProgram>
register_only_corpus()
{
  return {
      {"arithmetic",
       {addi(1, 0, 10), addi(2, 0, 3), op(3, 1, 2, 0, kF7Base),
        op(4, 1, 2, 0, kF7Alt), kEcall}},
      {"logical_shift",
       {addi(1, 0, 0xF0), addi(2, 0, 4), op(3, 1, 1, 4, kF7Base),
        op(4, 1, 2, 6, kF7Base), op(5, 1, 2, 1, kF7Base),
        op(6, 1, 2, 5, kF7Base), kEcall}},
      {"muldiv",
       {addi(1, 0, 20), addi(2, 0, 6), op(3, 1, 2, 0, kF7MulDiv),
        op(4, 1, 2, 4, kF7MulDiv), op(5, 1, 2, 6, kF7MulDiv), kEcall}},
      {"signed_div_and_sra",
       {addi(1, 0, -8), addi(2, 0, 2), op(3, 1, 2, 5, kF7Alt),
        op(4, 1, 2, 4, kF7MulDiv), kEcall}},
      {"branch_taken",
       {addi(1, 0, 5), addi(2, 0, 5), branch(0, 1, 2, 8), addi(3, 0, 99),
        addi(4, 0, 77), kEcall}},
      {"branch_not_taken",
       {addi(1, 0, 5), addi(2, 0, 6), branch(0, 1, 2, 8), addi(3, 0, 99),
        addi(4, 0, 77), kEcall}},
      {"jal_forward",
       {addi(1, 0, 1), j_type(0, 8), addi(2, 0, 99), addi(3, 0, 7), kEcall}},
  };
}

TEST(Rv32SpikeOracle, handCorpusMatchesSpike)
{
  if (!spike_oracle_available()) {
    GTEST_SKIP() << "RV32_ORACLE_BIN not set or not executable";
  }
  const std::optional<std::string> binary = spike_oracle_binary();
  ASSERT_TRUE(binary.has_value());
  SpikeOracle spike(binary.value());
  MachineOracle machine;
  SpecModelOracle spec;

  for (const NamedProgram &prog : register_only_corpus()) {
    SCOPED_TRACE(prog.name);
    expect_oracles_agree_with_spike(spike, machine, spec,
                                    reg_program(prog.words));
  }
}

TEST(Rv32SpikeOracle, randomCorpusMatchesSpike)
{
  if (!spike_oracle_available()) {
    GTEST_SKIP() << "RV32_ORACLE_BIN not set or not executable";
  }
  const std::optional<std::string> binary = spike_oracle_binary();
  ASSERT_TRUE(binary.has_value());
  SpikeOracle spike(binary.value());
  MachineOracle machine;
  SpecModelOracle spec;

  // Same generator as the fast non-gated Machine-vs-spec pass in the oracle
  // test, here checked against the independent Spike reference. Fewer seeds
  // because each Spike run costs the subprocess timeout.
  for (uint64_t seed = 0; seed < 24; ++seed) {
    SCOPED_TRACE(seed);
    DifferentialProgram program;
    program.code =
        test_corpus::generate_register_only_program(0xD1FFULL + seed, 48);
    program.memory_bytes = 256;
    program.initial_memory.assign(256, 0);
    expect_oracles_agree_with_spike(spike, machine, spec, program);
  }
}

} // namespace
} // namespace hogd::rv32
