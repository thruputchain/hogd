#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/test_support.h"

namespace hogd::rv32 {
namespace {

void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

std::vector<uint8_t>
code_from_words(std::initializer_list<uint32_t> words)
{
  std::vector<uint8_t> code;
  code.reserve(words.size() * kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(code, word);
  }
  return code;
}

std::shared_ptr<const Program>
make_program(std::initializer_list<uint32_t> words)
{
  const auto code = code_from_words(words);
  auto result =
      Program::create(std::span<const uint8_t>(code.data(), code.size()));
  EXPECT_TRUE(result.has_value());
  return std::make_shared<Program>(std::move(*result));
}

uint32_t
r_type(uint8_t funct7, uint8_t rs2, uint8_t rs1, uint8_t funct3, uint8_t rd)
{
  return uint32_t{funct7} << 25 | uint32_t{rs2} << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | uint32_t{rd} << 7 | 0x33U;
}

uint32_t
i_type(uint32_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  return (uint32_t(imm) & 0xfffU) << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | uint32_t{rd} << 7 | opcode;
}

uint32_t
s_type(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  const uint32_t uimm = uint32_t(imm) & 0xfffU;
  return (uimm >> 5) << 25 | uint32_t{rs2} << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | (uimm & 0x1fU) << 7 | 0x23U;
}

uint32_t
b_type(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  const uint32_t uimm = uint32_t(imm) & 0x1fffU;
  return ((uimm >> 12) & 0x1U) << 31 | ((uimm >> 5) & 0x3fU) << 25 |
         uint32_t{rs2} << 20 | uint32_t{rs1} << 15 | uint32_t{funct3} << 12 |
         ((uimm >> 1) & 0xfU) << 8 | ((uimm >> 11) & 0x1U) << 7 | 0x63U;
}

uint32_t
u_type(uint32_t opcode, uint8_t rd, uint32_t imm)
{
  return (imm & 0xfffff000U) | uint32_t{rd} << 7 | opcode;
}

uint32_t
j_type(uint8_t rd, int32_t imm)
{
  const uint32_t uimm = uint32_t(imm) & 0x1fffffU;
  return ((uimm >> 20) & 0x1U) << 31 | ((uimm >> 1) & 0x3ffU) << 21 |
         ((uimm >> 11) & 0x1U) << 20 | ((uimm >> 12) & 0xffU) << 12 |
         uint32_t{rd} << 7 | 0x6fU;
}

uint32_t
addi(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x13, rd, 0, rs1, imm);
}

uint32_t
load(uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  return i_type(0x03, rd, funct3, rs1, imm);
}

uint32_t
jalr(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x67, rd, 0, rs1, imm);
}

struct HaltHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    ++calls;
    last_args = args;
    last_pc = view.pc();
    return HostResult::halt(77);
  }

  int calls = 0;
  EcallArgs last_args;
  uint32_t last_pc = 0;
};

struct ContinueThenHaltHost : Host {
  HostResult ecall(MachineView, const EcallArgs &) noexcept override
  {
    ++calls;
    return calls == 1 ? HostResult::continue_with(123) : HostResult::halt();
  }

  int calls = 0;
};

struct MemoryHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &) noexcept override
  {
    const std::array<uint8_t, 4> data = {1, 2, 3, 4};
    wrote = view.write_memory(8, data);
    return HostResult::halt();
  }

  bool wrote = false;
};

struct TrapHost : Host {
  HostResult ecall(MachineView, const EcallArgs &) noexcept override
  {
    return HostResult::trap();
  }
};

TEST(Rv32ProgramTest, smoke_builds_standalone_vm)
{
  const auto program = make_program({addi(0, 0, 0)});
  Machine machine(program, 4096, 10);
  EXPECT_EQ(machine.pc(), 0U);
  EXPECT_EQ(machine.reg(0), 0U);
  EXPECT_EQ(machine.reg(2), 4096U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
}

TEST(Rv32ProgramTest, validates_code_shape_and_profile)
{
  {
    const std::array<uint8_t, 1> code = {0};
    EXPECT_EQ(Program::validate(code).error, ProgramError::kCodeSizeNotAligned);
  }
  {
    std::vector<uint8_t> code(kMaxCodeBytes + kInstructionBytes);
    EXPECT_EQ(Program::validate(code).error, ProgramError::kCodeTooLarge);
  }
  {
    const auto code = code_from_words({0x00000001U});
    EXPECT_EQ(Program::validate(code).error,
              ProgramError::kUnsupportedCompressedInstruction);
  }
  {
    const auto code = code_from_words({0x00100073U});
    EXPECT_EQ(Program::validate(code).error,
              ProgramError::kUnsupportedSystemInstruction);
  }
  {
    const auto code = code_from_words({0x0000100fU});
    EXPECT_EQ(Program::validate(code).error,
              ProgramError::kUnsupportedInstruction);
  }
  {
    const auto code = code_from_words({0x0000007fU});
    EXPECT_EQ(Program::validate(code).error,
              ProgramError::kUnsupportedInstruction);
  }
}

TEST(Rv32ProgramTest, create_returns_validated_immutable_program)
{
  static_assert(!std::is_default_constructible_v<Program>);

  const auto valid = code_from_words({addi(1, 0, 7)});
  auto program = Program::create(valid);
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(program->instruction_count(), 1U);
  EXPECT_EQ(program->byte_size(), kInstructionBytes);

  const std::array<uint8_t, 1> invalid = {0};
  auto error = Program::create(invalid);
  ASSERT_FALSE(error.has_value());
  EXPECT_EQ(error.error(), ProgramError::kCodeSizeNotAligned);
}

TEST(Rv32HostViewTest, checks_memory_ranges_with_machine_invariants)
{
  const auto program = make_program({addi(0, 0, 0)});
  Machine machine(program, 16, 10);
  MachineView view = machine.view();

  std::array<uint8_t, 4> data = {1, 2, 3, 4};
  EXPECT_TRUE(view.write_memory(12, data));
  EXPECT_EQ(machine.memory()[12], 1U);
  EXPECT_EQ(machine.memory()[15], 4U);

  std::array<uint8_t, 4> out = {};
  EXPECT_TRUE(view.read_memory(12, out));
  EXPECT_EQ(out, data);

  std::array<uint8_t, 1> one_byte = {9};
  EXPECT_TRUE(
      view.write_memory(16, std::span<const uint8_t>(one_byte.data(), 0)));
  EXPECT_TRUE(view.read_memory(16, std::span<uint8_t>(one_byte.data(), 0)));
  EXPECT_FALSE(view.write_memory(16, one_byte));
  EXPECT_FALSE(view.write_memory(13, data));
  EXPECT_FALSE(view.read_memory(13, out));
  EXPECT_FALSE(
      view.write_memory(std::numeric_limits<uint32_t>::max(), one_byte));
  EXPECT_FALSE(view.read_memory(std::numeric_limits<uint32_t>::max(), out));
}

TEST(Rv32DecodeTest, decodes_representative_instruction_families)
{
  struct Case {
    uint32_t word;
    InstructionKind kind;
    Op op;
    int rd;
    int rs1;
    int rs2;
    int32_t immediate;
    uint64_t gas_cost;
  };

  const std::array<Case, 12> cases = {{
      {u_type(0x37, 5, 0x12345000U), InstructionKind::kLui, Op::kLui, 5, -1, -1,
       0x12345000, kBaseInstructionGas},
      {u_type(0x17, 6, 0xfffff000U), InstructionKind::kAuipc, Op::kAuipc, 6, -1,
       -1, -4096, kBaseInstructionGas},
      {j_type(7, -2048), InstructionKind::kJal, Op::kJal, 7, -1, -1, -2048,
       kBaseInstructionGas},
      {jalr(8, 9, 12), InstructionKind::kJalr, Op::kJalr, 8, 9, 12, 12,
       kBaseInstructionGas},
      {b_type(0, 1, 2, -16), InstructionKind::kBranch, Op::kBeq, -1, 1, 2, -16,
       kBaseInstructionGas},
      {load(3, 2, 4, 20), InstructionKind::kLoad, Op::kLw, 3, 4, 20, 20,
       kBaseInstructionGas},
      {s_type(2, 5, 6, -12), InstructionKind::kStore, Op::kSw, -1, 5, 6, -12,
       kBaseInstructionGas},
      {addi(10, 11, -1), InstructionKind::kOpImm, Op::kAddi, 10, 11, 31, -1,
       kBaseInstructionGas},
      {r_type(0, 13, 12, 0, 14), InstructionKind::kOp, Op::kAdd, 14, 12, 13, 0,
       kBaseInstructionGas},
      {r_type(1, 13, 12, 4, 14), InstructionKind::kOp, Op::kDiv, 14, 12, 13, 0,
       kDivInstructionGas},
      {0x0000000fU, InstructionKind::kFence, Op::kFence, 0, 0, 0, 0,
       kBaseInstructionGas},
      {0x00000073U, InstructionKind::kEcall, Op::kEcall, 0, 0, 0, 0,
       kBaseInstructionGas},
  }};

  for (const auto &test_case : cases) {
    const auto result = decode_instruction(test_case.word);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.instruction.kind, test_case.kind);
    EXPECT_EQ(result.instruction.op, test_case.op);
    if (test_case.rd >= 0) {
      EXPECT_EQ(result.instruction.rd, test_case.rd);
    }
    if (test_case.rs1 >= 0) {
      EXPECT_EQ(result.instruction.rs1, test_case.rs1);
    }
    if (test_case.rs2 >= 0) {
      EXPECT_EQ(result.instruction.rs2, test_case.rs2);
    }
    EXPECT_EQ(result.instruction.immediate, test_case.immediate);
    EXPECT_EQ(result.instruction.gas_cost, test_case.gas_cost);
  }
}

TEST(Rv32GasTest, version_pins_full_cost_table)
{
  std::array<uint64_t, kOpCount> expected = {};
  expected.fill(1);
  expected[static_cast<size_t>(Op::kMul)] = 3;
  expected[static_cast<size_t>(Op::kMulh)] = 3;
  expected[static_cast<size_t>(Op::kMulhsu)] = 3;
  expected[static_cast<size_t>(Op::kMulhu)] = 3;
  expected[static_cast<size_t>(Op::kDiv)] = 8;
  expected[static_cast<size_t>(Op::kDivu)] = 8;
  expected[static_cast<size_t>(Op::kRem)] = 8;
  expected[static_cast<size_t>(Op::kRemu)] = 8;

  EXPECT_EQ(kGasTableVersion, 1U);
  EXPECT_EQ(kInstructionGasCosts, expected);
}

TEST(Rv32ExecutionTest, keeps_x0_hardwired_and_uses_aligned_stack)
{
  const auto program = make_program({addi(0, 0, 123), addi(1, 0, 7)});
  Machine machine(program, 31, 10);
  EXPECT_EQ(machine.reg(2), 16U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(0), 0U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(1), 7U);
}

TEST(Rv32ExecutionTest, executes_arithmetic_logic_and_shift_operations)
{
  const auto program = make_program({
      r_type(0, 2, 1, 0, 3),      // add
      r_type(0x20, 2, 1, 0, 4),   // sub
      r_type(0, 2, 1, 1, 5),      // sll
      r_type(0, 2, 1, 2, 6),      // slt
      r_type(0, 2, 1, 3, 7),      // sltu
      r_type(0, 2, 1, 4, 8),      // xor
      r_type(0, 2, 1, 5, 9),      // srl
      r_type(0x20, 2, 1, 5, 10),  // sra
      r_type(0, 2, 1, 6, 11),     // or
      r_type(0, 2, 1, 7, 12),     // and
      i_type(0x13, 13, 1, 1, 31), // slli
  });
  Machine machine(program, 1024, 100);
  MachineTestPeer::set_reg(machine, 1, 0x80000001U);
  MachineTestPeer::set_reg(machine, 2, 33);

  for (int i = 0; i < 11; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }

  EXPECT_EQ(machine.reg(3), 0x80000022U);
  EXPECT_EQ(machine.reg(4), 0x7fffffe0U);
  EXPECT_EQ(machine.reg(5), 0x00000002U);
  EXPECT_EQ(machine.reg(6), 1U);
  EXPECT_EQ(machine.reg(7), 0U);
  EXPECT_EQ(machine.reg(8), 0x80000020U);
  EXPECT_EQ(machine.reg(9), 0x40000000U);
  EXPECT_EQ(machine.reg(10), 0xc0000000U);
  EXPECT_EQ(machine.reg(11), 0x80000021U);
  EXPECT_EQ(machine.reg(12), 0x00000001U);
  EXPECT_EQ(machine.reg(13), 0x80000000U);
}

TEST(Rv32ExecutionTest, masks_register_shift_amounts)
{
  const auto program = make_program({
      r_type(0, 2, 1, 1, 3),    // sll
      r_type(0, 2, 1, 5, 4),    // srl
      r_type(0x20, 2, 1, 5, 5), // sra
  });
  Machine machine(program, 1024, 10);
  MachineTestPeer::set_reg(machine, 1, 0x80000001U);
  MachineTestPeer::set_reg(machine, 2, 40);

  ASSERT_EQ(machine.step(), StepResult::kContinue);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(3), 0x00000100U);
  EXPECT_EQ(machine.reg(4), 0x00800000U);
  EXPECT_EQ(machine.reg(5), 0xff800000U);
}

TEST(Rv32ExecutionTest, executes_lui_and_auipc_values)
{
  const auto program = make_program(
      {u_type(0x37, 1, 0x12345000U), u_type(0x17, 2, 0x00001000U)});
  Machine machine(program, 1024, 10);

  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(1), 0x12345000U);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(2), 0x00001004U);
}

TEST(Rv32ExecutionTest, sign_and_zero_extends_loads)
{
  const auto program = make_program({
      addi(1, 0, 16),
      addi(2, 0, -1),
      s_type(2, 1, 2, 0), // sw
      load(3, 0, 1, 0),   // lb
      load(4, 4, 1, 0),   // lbu
      load(5, 1, 1, 0),   // lh
      load(6, 5, 1, 0),   // lhu
      load(7, 2, 1, 0),   // lw
  });
  Machine machine(program, 64, 100);
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(machine.reg(3), 0xffffffffU);
  EXPECT_EQ(machine.reg(4), 0xffU);
  EXPECT_EQ(machine.reg(5), 0xffffffffU);
  EXPECT_EQ(machine.reg(6), 0xffffU);
  EXPECT_EQ(machine.reg(7), 0xffffffffU);
}

TEST(Rv32ExecutionTest, branches_and_jumps)
{
  const auto program = make_program({
      addi(1, 0, 1),
      b_type(0, 1, 1, 8), // beq skips next addi
      addi(2, 0, 1),
      addi(2, 0, 2),
      j_type(3, 8), // jal skips next addi
      addi(4, 0, 1),
      addi(4, 0, 4),
  });
  Machine machine(program, 1024, 100);
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(machine.reg(2), 2U);
  EXPECT_EQ(machine.reg(3), 20U);
  EXPECT_EQ(machine.reg(4), 4U);
}

TEST(Rv32ExecutionTest, bad_jump_targets_trap_before_state_mutation)
{
  {
    const auto program = make_program({j_type(1, 2), addi(2, 0, 1)});
    Machine machine(program, 1024, 100);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
    EXPECT_EQ(machine.gas_remaining(), 99U);
  }
  {
    const auto program = make_program({jalr(1, 2, 0), addi(3, 0, 1)});
    Machine machine(program, 1024, 100);
    MachineTestPeer::set_reg(machine, 2, 3);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
  }
  {
    const auto program =
        make_program({addi(1, 0, 1), b_type(0, 1, 1, 2), addi(2, 0, 1)});
    Machine machine(program, 1024, 100);
    ASSERT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
    EXPECT_EQ(machine.pc(), 4U);
  }
}

TEST(Rv32ExecutionTest, jalr_clears_low_target_bit)
{
  const auto program =
      make_program({jalr(2, 1, 0), addi(3, 0, 1), addi(3, 0, 2)});
  Machine machine(program, 1024, 100);
  MachineTestPeer::set_reg(machine, 1, 9);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.pc(), 8U);
  EXPECT_EQ(machine.reg(2), 4U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(3), 2U);
}

TEST(Rv32ExecutionTest, traps_on_bad_fetch_memory_and_alignment)
{
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, 1024, 10);
    MachineTestPeer::set_pc(machine, 2);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
  }
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, 1024, 10);
    MachineTestPeer::set_pc(machine, 4);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
  }
  {
    const auto program = make_program({load(1, 2, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 2, 1);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kMisalignedAccess);
  }
  {
    const auto program = make_program({load(1, 2, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 2, 16);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadMemory);
  }
  {
    const auto program = make_program({s_type(2, 1, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 1, 2);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kMisalignedAccess);
  }
  {
    const auto program = make_program({s_type(2, 1, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 1, 16);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadMemory);
  }
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, kMaxMemoryBytes + 1, 10);
    EXPECT_EQ(machine.trap(), Trap::kBadMemory);
    EXPECT_TRUE(machine.halted());
  }
}

TEST(Rv32ExecutionTest, running_off_end_traps_as_bad_pc)
{
  const auto program = make_program({addi(1, 0, 1)});
  Machine machine(program, 1024, 10);

  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.pc(), 4U);
  EXPECT_EQ(machine.step(), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
}

TEST(Rv32ExecutionTest, implements_m_extension_edge_cases)
{
  const auto program = make_program({
      r_type(1, 2, 1, 0, 3),  // mul
      r_type(1, 2, 1, 1, 4),  // mulh
      r_type(1, 2, 1, 2, 5),  // mulhsu
      r_type(1, 2, 1, 3, 6),  // mulhu
      r_type(1, 2, 1, 4, 7),  // div
      r_type(1, 2, 1, 5, 8),  // divu
      r_type(1, 2, 1, 6, 9),  // rem
      r_type(1, 2, 1, 7, 10), // remu
  });
  Machine machine(program, 1024, 100);
  MachineTestPeer::set_reg(machine, 1, 0x80000000U);
  MachineTestPeer::set_reg(machine, 2, 0xffffffffU);
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(machine.reg(3), 0x80000000U);
  EXPECT_EQ(machine.reg(4), 0U);
  EXPECT_EQ(machine.reg(5), 0x80000000U);
  EXPECT_EQ(machine.reg(6), 0x7fffffffU);
  EXPECT_EQ(machine.reg(7), 0x80000000U);
  EXPECT_EQ(machine.reg(8), 0U);
  EXPECT_EQ(machine.reg(9), 0U);
  EXPECT_EQ(machine.reg(10), 0x80000000U);

  const auto div_zero = make_program({
      r_type(1, 2, 1, 4, 3),
      r_type(1, 2, 1, 5, 4),
      r_type(1, 2, 1, 6, 5),
      r_type(1, 2, 1, 7, 6),
  });
  Machine div_zero_machine(div_zero, 1024, 100);
  MachineTestPeer::set_reg(div_zero_machine, 1, 123);
  MachineTestPeer::set_reg(div_zero_machine, 2, 0);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(div_zero_machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(div_zero_machine.reg(3), 0xffffffffU);
  EXPECT_EQ(div_zero_machine.reg(4), 0xffffffffU);
  EXPECT_EQ(div_zero_machine.reg(5), 123U);
  EXPECT_EQ(div_zero_machine.reg(6), 123U);
}

TEST(Rv32ExecutionTest, debits_gas_before_later_traps_and_mutation)
{
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, 1024, 0);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
  }
  {
    const auto program = make_program({load(1, 2, 2, 0)});
    Machine machine(program, 16, kBaseInstructionGas);
    MachineTestPeer::set_reg(machine, 2, 1);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kMisalignedAccess);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
  }
}

TEST(Rv32GasTest, charges_mul_and_div_costs_before_mutation)
{
  {
    const auto program = make_program({r_type(1, 2, 1, 0, 3)});
    Machine machine(program, 1024, kMulInstructionGas - 1);
    MachineTestPeer::set_reg(machine, 1, 6);
    MachineTestPeer::set_reg(machine, 2, 7);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.reg(3), 0U);
    EXPECT_EQ(machine.gas_remaining(), kMulInstructionGas - 1);
  }
  {
    const auto program = make_program({r_type(1, 2, 1, 0, 3)});
    Machine machine(program, 1024, kMulInstructionGas);
    MachineTestPeer::set_reg(machine, 1, 6);
    MachineTestPeer::set_reg(machine, 2, 7);
    EXPECT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.reg(3), 42U);
    EXPECT_EQ(machine.gas_remaining(), 0U);
  }
  {
    const auto program = make_program({r_type(1, 2, 1, 4, 3)});
    Machine machine(program, 1024, kDivInstructionGas - 1);
    MachineTestPeer::set_reg(machine, 1, 42);
    MachineTestPeer::set_reg(machine, 2, 6);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.reg(3), 0U);
    EXPECT_EQ(machine.gas_remaining(), kDivInstructionGas - 1);
  }
  {
    const auto program = make_program({r_type(1, 2, 1, 4, 3)});
    Machine machine(program, 1024, kDivInstructionGas);
    MachineTestPeer::set_reg(machine, 1, 42);
    MachineTestPeer::set_reg(machine, 2, 6);
    EXPECT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.reg(3), 7U);
    EXPECT_EQ(machine.gas_remaining(), 0U);
  }
}

TEST(Rv32ExecutionTest, handles_ecall_step_completion_and_run)
{
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, kBaseInstructionGas);
    MachineTestPeer::set_reg(machine, 10, 1);
    MachineTestPeer::set_reg(machine, 17, 99);
    EXPECT_EQ(machine.step(), StepResult::kEcall);
    ASSERT_TRUE(machine.pending_ecall().has_value());
    EXPECT_EQ(machine.pending_ecall()->a0, 1U);
    EXPECT_EQ(machine.pending_ecall()->a7, 99U);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.complete_ecall(HostResult::continue_with(55)),
              StepResult::kContinue);
    EXPECT_EQ(machine.reg(10), 55U);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, 100);
    MachineTestPeer::set_reg(machine, 10, 7);
    MachineTestPeer::set_reg(machine, 17, 42);
    HaltHost host;
    EXPECT_EQ(machine.run(host), StepResult::kHalted);
    EXPECT_EQ(host.calls, 1);
    EXPECT_EQ(host.last_args.a0, 7U);
    EXPECT_EQ(host.last_args.a7, 42U);
    EXPECT_EQ(host.last_pc, 4U);
    EXPECT_EQ(machine.trap(), Trap::kNone);
    EXPECT_EQ(machine.halt_code(), 77U);
  }
  {
    const auto program =
        make_program({0x00000073U, addi(1, 10, 1), 0x00000073U});
    Machine machine(program, 1024, 100);
    ContinueThenHaltHost host;
    EXPECT_EQ(machine.run(host), StepResult::kHalted);
    EXPECT_EQ(host.calls, 2);
    EXPECT_EQ(machine.reg(1), 124U);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, 100);
    TrapHost host;
    EXPECT_EQ(machine.run(host), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kHostTrap);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 100);
    MemoryHost host;
    EXPECT_EQ(machine.run(host), StepResult::kHalted);
    EXPECT_TRUE(host.wrote);
    EXPECT_EQ(machine.memory()[8], 1U);
    EXPECT_EQ(machine.memory()[11], 4U);
  }
}

TEST(Rv32ConformanceTest, runs_representative_loop_program)
{
  const auto program = make_program({
      addi(1, 0, 5),         // counter
      addi(2, 0, 0),         // sum
      r_type(0, 1, 2, 0, 2), // sum += counter
      addi(1, 1, -1),        // --counter
      b_type(1, 1, 0, -8),   // loop while counter != 0
      0x00000073U,           // ecall halt
  });
  Machine machine(program, 1024, 100);
  HaltHost host;
  EXPECT_EQ(machine.run(host), StepResult::kHalted);
  EXPECT_EQ(machine.reg(2), 15U);
  EXPECT_EQ(host.calls, 1);
}

TEST(Rv32ConformanceTest, randomized_store_bounds_are_deterministic)
{
  const auto program = make_program({s_type(2, 1, 2, 0)});
  std::mt19937 rng(0x12345678);
  std::uniform_int_distribution<uint32_t> address_dist(0, 40);

  for (int i = 0; i < 128; ++i) {
    const uint32_t address = address_dist(rng);
    Machine machine(program, 32, 10);
    MachineTestPeer::set_reg(machine, 1, address);
    MachineTestPeer::set_reg(machine, 2, 0xaabbccddU);
    const StepResult result = machine.step();
    if ((address % 4) != 0) {
      EXPECT_EQ(result, StepResult::kTrap);
      EXPECT_EQ(machine.trap(), Trap::kMisalignedAccess);
    }
    else if (!memory_range_in_bounds(machine.memory().size(), address, 4)) {
      EXPECT_EQ(result, StepResult::kTrap);
      EXPECT_EQ(machine.trap(), Trap::kBadMemory);
    }
    else {
      ASSERT_EQ(result, StepResult::kContinue);
      const auto memory = machine.memory();
      EXPECT_EQ(memory[address], 0xddU);
      EXPECT_EQ(memory[address + 1], 0xccU);
      EXPECT_EQ(memory[address + 2], 0xbbU);
      EXPECT_EQ(memory[address + 3], 0xaaU);
    }
  }
}

} // namespace
} // namespace hogd::rv32
