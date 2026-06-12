#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/hogexec/runtime/rv32/test_support.h"

// NOLINTBEGIN(google-readability-avoid-underscore-in-googletest-name,modernize-use-designated-initializers)

namespace hogd::rv32 {
namespace {

using test_program::addi;
using test_program::b_type;
using test_program::i_type;
using test_program::j_type;
using test_program::jalr;
using test_program::load;
using test_program::make_program;
using test_program::r_type;
using test_program::s_type;
using test_program::u_type;

struct RegValue {
  size_t index = 0;
  uint32_t value = 0;
};

struct MemoryValue {
  uint32_t address = 0;
  std::vector<uint8_t> bytes;
};

struct ConformanceCase {
  std::string name;
  std::vector<uint32_t> code;
  uint32_t memory_bytes = 256;
  uint64_t gas_limit = 1'000;
  uint32_t initial_pc = 0;
  std::vector<RegValue> initial_regs;
  std::vector<MemoryValue> initial_memory;
  StepResult expected_result = StepResult::kHalted;
  Trap expected_trap = Trap::kNone;
  bool expected_halted = true;
  uint32_t expected_halt_code = 0;
  bool check_pc = true;
  uint32_t expected_pc = 0;
  bool check_gas_consumed = false;
  uint64_t expected_gas_consumed = 0;
  std::vector<RegValue> expected_regs;
  std::vector<MemoryValue> expected_memory;
};

class HaltWithA0Host final : public Host {
public:
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)args;
    return HostResult::halt(view.reg(10));
  }
};

StepResult
run_to_terminal(Machine &machine, Host &host)
{
  for (size_t steps = 0; steps < 1024; ++steps) {
    const StepResult result = machine.step();
    switch (result) {
    case StepResult::kContinue:
      continue;
    case StepResult::kEcall:
    {
      const auto pending = machine.pending_ecall();
      if (!pending.has_value()) {
        return StepResult::kTrap;
      }
      const StepResult completed =
          machine.complete_ecall(host.ecall(machine.view(), pending.value()));
      if (completed == StepResult::kContinue) {
        continue;
      }
      return completed;
    }
    case StepResult::kHalted:
    case StepResult::kTrap:
      return result;
    default:
      return StepResult::kTrap;
    }
  }
  return StepResult::kTrap;
}

std::vector<ConformanceCase>
conformance_cases()
{
  return {
      {.name = "arithmetic_and_logic",
       .code = {addi(1, 0, 7), addi(2, 0, -3), r_type(0, 2, 1, 0, 3),
                r_type(0x20, 2, 1, 0, 4), r_type(0, 2, 1, 2, 5),
                r_type(0, 2, 1, 3, 6), r_type(0, 2, 1, 4, 7),
                r_type(0, 2, 1, 6, 8), r_type(0, 2, 1, 7, 9), 0x00000073U},
       .expected_pc = 40,
       .check_gas_consumed = true,
       .expected_gas_consumed = 10,
       .expected_regs = {{3, 4},
                         {4, 10},
                         {5, 0},
                         {6, 1},
                         {7, 0xfffffffaU},
                         {8, 0xffffffffU},
                         {9, 5}}},
      {.name = "shift_immediates_and_registers",
       .code = {addi(1, 0, -8), addi(2, 0, 2), i_type(0x13U, 3, 1, 1, 1),
                i_type(0x13U, 4, 5, 1, 1), i_type(0x13U, 5, 5, 1, 0x401),
                r_type(0, 2, 1, 1, 6), r_type(0, 2, 1, 5, 7),
                r_type(0x20, 2, 1, 5, 8), addi(9, 0, -32),
                r_type(0, 9, 1, 1, 13), r_type(0, 9, 1, 5, 14),
                r_type(0x20, 9, 1, 5, 15), 0x00000073U},
       .expected_pc = 52,
       .expected_regs = {{3, 0xfffffff0U},
                         {4, 0x7ffffffcU},
                         {5, 0xfffffffcU},
                         {6, 0xffffffe0U},
                         {7, 0x3ffffffeU},
                         {8, 0xfffffffeU},
                         {13, 0xfffffff8U},
                         {14, 0xfffffff8U},
                         {15, 0xfffffff8U}}},
      {.name = "branch_jump_and_upper_immediates",
       .code = {addi(1, 0, 1), b_type(0, 1, 1, 8), addi(2, 0, 99), j_type(3, 8),
                addi(2, 0, 2), u_type(0x17U, 4, 0x00001000U), 0x00000073U},
       .initial_regs = {{2, 0}},
       .expected_pc = 28,
       .expected_regs = {{2, 0}, {3, 16}, {4, 0x00001014U}}},
      {.name = "little_endian_load_store_sign_extension",
       .code = {addi(1, 0, 32), u_type(0x37U, 2, 0xdeadc000U), addi(2, 2, -273),
                s_type(2, 1, 2, 0), load(3, 0, 1, 0), load(4, 4, 1, 0),
                load(5, 1, 1, 0), load(6, 5, 1, 0), load(7, 2, 1, 0),
                0x00000073U},
       .expected_pc = 40,
       .expected_regs = {{3, 0xffffffefU},
                         {4, 0x000000efU},
                         {5, 0xffffbeefU},
                         {6, 0x0000beefU},
                         {7, 0xdeadbeefU}},
       .expected_memory = {{32, {0xef, 0xbe, 0xad, 0xde}}}},
      {.name = "rv32m_division_edge_cases",
       .code = {addi(1, 0, -7), addi(2, 0, 3), r_type(1, 2, 1, 0, 3),
                r_type(1, 2, 1, 4, 4), r_type(1, 2, 1, 6, 5),
                r_type(1, 0, 1, 4, 6), r_type(1, 0, 1, 6, 7),
                u_type(0x37U, 8, 0x80000000U), addi(9, 0, -1),
                r_type(1, 9, 8, 4, 10), r_type(1, 9, 8, 6, 11),
                r_type(1, 0, 1, 5, 12), r_type(1, 0, 1, 7, 13), 0x00000073U},
       .expected_halt_code = 0x80000000U,
       .expected_pc = 56,
       .expected_regs = {{3, 0xffffffebU},
                         {4, 0xfffffffeU},
                         {5, 0xffffffffU},
                         {6, 0xffffffffU},
                         {7, 0xfffffff9U},
                         {10, 0x80000000U},
                         {11, 0},
                         {12, 0xffffffffU},
                         {13, 0xfffffff9U}}},
      {.name = "jalr_masks_low_target_bit",
       .code = {addi(1, 0, 9), jalr(2, 1, 0), 0x00000073U},
       .expected_pc = 12,
       .expected_regs = {{2, 8}}},
      {.name = "fence_is_ordering_noop_in_single_threaded_vm",
       .code = {addi(1, 0, 1), 0x0000000fU, addi(2, 1, 2), 0x00000073U},
       .expected_pc = 16,
       .check_gas_consumed = true,
       .expected_gas_consumed = 4,
       .expected_regs = {{1, 1}, {2, 3}}},
      {.name = "ebreak_is_deterministic_runtime_trap",
       .code = {addi(1, 0, 7), 0x00100073U, addi(1, 0, 9)},
       .expected_result = StepResult::kTrap,
       .expected_trap = Trap::kBreakpoint,
       .expected_halted = true,
       .expected_pc = 4,
       .check_gas_consumed = true,
       .expected_gas_consumed = 2,
       .expected_regs = {{1, 7}}},
      {.name = "unaligned_load_reassembles_little_endian",
       .code = {addi(1, 0, 32), u_type(0x37U, 2, 0xdeadc000U), addi(2, 2, -273),
                s_type(2, 1, 2, 0), addi(3, 1, 1), load(4, 2, 3, 0),
                0x00000073U},
       .expected_pc = 28,
       .check_gas_consumed = true,
       .expected_gas_consumed = 7,
       .expected_regs = {{4, 0x00deadbeU}},
       .expected_memory = {{32, {0xef, 0xbe, 0xad, 0xde}}}},
      {.name = "unaligned_store_mutates_exactly_four_bytes",
       .code = {addi(1, 0, 33), u_type(0x37U, 2, 0xdeadc000U), addi(2, 2, -273),
                s_type(2, 1, 2, 0), 0x00000073U},
       .initial_memory = {{32, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                0x11}}},
       .expected_pc = 20,
       .check_gas_consumed = true,
       .expected_gas_consumed = 5,
       .expected_memory = {{32, {0x11, 0xef, 0xbe, 0xad, 0xde, 0x11, 0x11,
                                 0x11}}}},
      {.name = "unaligned_out_of_bounds_load_traps_bad_memory",
       .code = {addi(1, 0, 253), load(2, 2, 1, 0), 0x00000073U},
       .initial_regs = {{2, 0}},
       .expected_result = StepResult::kTrap,
       .expected_trap = Trap::kBadMemory,
       .expected_halted = true,
       .expected_pc = 4,
       .check_gas_consumed = true,
       .expected_gas_consumed = 2,
       .expected_regs = {{2, 0}}},
      {.name = "taken_branch_to_bad_pc_traps_after_gas",
       .code = {addi(1, 0, 1), b_type(0, 1, 1, 12), addi(2, 0, 1)},
       .initial_regs = {{2, 0}},
       .expected_result = StepResult::kTrap,
       .expected_trap = Trap::kBadPc,
       .expected_halted = true,
       .expected_pc = 4,
       .check_gas_consumed = true,
       .expected_gas_consumed = 2,
       .expected_regs = {{2, 0}}},
      {.name = "out_of_gas_has_no_partial_mutation",
       .code = {addi(1, 0, 1), 0x00000073U},
       .gas_limit = 0,
       .expected_result = StepResult::kTrap,
       .expected_trap = Trap::kOutOfGas,
       .expected_halted = true,
       .expected_pc = 0,
       .check_gas_consumed = true,
       .expected_gas_consumed = 0,
       .expected_regs = {{1, 0}}},
  };
}

std::string
case_name(const testing::TestParamInfo<ConformanceCase> &info)
{
  std::string name = info.param.name;
  std::ranges::replace_if(
      name,
      [](char ch) { return !std::isalnum(static_cast<unsigned char>(ch)); },
      '_');
  return name;
}

class Rv32ConformanceTest : public testing::TestWithParam<ConformanceCase> {};

TEST_P(Rv32ConformanceTest, matchesExpectedMachineState)
{
  const ConformanceCase &test_case = GetParam();
  const auto program = make_program(
      std::span<const uint32_t>(test_case.code.data(), test_case.code.size()));
  ASSERT_NE(program, nullptr);

  Machine machine(program, test_case.memory_bytes, test_case.gas_limit,
                  test_case.initial_pc);
  for (const RegValue &reg : test_case.initial_regs) {
    MachineTestPeer::set_reg(machine, reg.index, reg.value);
  }
  for (const MemoryValue &memory : test_case.initial_memory) {
    ASSERT_TRUE(machine.view().write_memory(memory.address, memory.bytes));
  }

  HaltWithA0Host host;
  const StepResult result = run_to_terminal(machine, host);

  EXPECT_EQ(result, test_case.expected_result) << test_case.name;
  EXPECT_EQ(machine.trap(), test_case.expected_trap) << test_case.name;
  EXPECT_EQ(machine.halted(), test_case.expected_halted) << test_case.name;
  if (test_case.expected_result == StepResult::kHalted) {
    EXPECT_EQ(machine.halt_code(), test_case.expected_halt_code)
        << test_case.name;
  }
  if (test_case.check_pc) {
    EXPECT_EQ(machine.pc(), test_case.expected_pc) << test_case.name;
  }
  if (test_case.check_gas_consumed) {
    EXPECT_EQ(test_case.gas_limit - machine.gas_remaining(),
              test_case.expected_gas_consumed)
        << test_case.name;
  }
  for (const RegValue &reg : test_case.expected_regs) {
    EXPECT_EQ(machine.reg(reg.index), reg.value)
        << test_case.name << " x" << reg.index;
  }
  for (const MemoryValue &expected : test_case.expected_memory) {
    ASSERT_TRUE(memory_range_in_bounds(machine.memory().size(),
                                       expected.address, expected.bytes.size()))
        << test_case.name;
    const auto actual =
        machine.memory().subspan(expected.address, expected.bytes.size());
    EXPECT_EQ(std::vector<uint8_t>(actual.begin(), actual.end()),
              expected.bytes)
        << test_case.name << " memory@" << expected.address;
  }
}

INSTANTIATE_TEST_SUITE_P(Core, Rv32ConformanceTest,
                         testing::ValuesIn(conformance_cases()), case_name);

} // namespace
} // namespace hogd::rv32

// NOLINTEND(google-readability-avoid-underscore-in-googletest-name,modernize-use-designated-initializers)
