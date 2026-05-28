#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/hogexec/runtime/rv32/test_support.h"

// Operand/result vectors adapted from the riscv-tests RV32UI/RV32UM test
// subtree vendored in PolkaVM commit bd2d14ed105467fd158b4744fd92f7045d288bb8.
// PolkaVM's root license is MIT/Apache-2.0; the borrowed test subtree carries
// its own Regents BSD-style notice. See docs/rv32/riscv-tests-license.md.
// NOLINTBEGIN(google-readability-avoid-underscore-in-googletest-name,modernize-use-designated-initializers)

namespace hogd::rv32 {
namespace {

using test_program::i_type;
using test_program::j_type;
using test_program::load;
using test_program::make_program;
using test_program::r_type;
using test_program::s_type;
using test_program::u_type;

constexpr uint32_t kEcall = 0x00000073U;
constexpr uint32_t kDataAddress = 64;
constexpr uint8_t kSentinelByte = 0xeeU;

class HaltHost final : public Host {
public:
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)view;
    (void)args;
    return HostResult::halt();
  }
};

struct RrOp {
  const char *name = "";
  uint8_t funct7 = 0;
  uint8_t funct3 = 0;
};

struct RrVector {
  std::string name;
  RrOp op;
  uint32_t lhs = 0;
  uint32_t rhs = 0;
  uint32_t expected = 0;
};

struct ImmOp {
  const char *name = "";
  uint8_t funct3 = 0;
  uint32_t imm_bits = 0;
};

struct ImmVector {
  std::string name;
  ImmOp op;
  uint32_t lhs = 0;
  uint32_t expected = 0;
};

struct BranchOp {
  const char *name = "";
  uint8_t funct3 = 0;
};

struct BranchVector {
  std::string name;
  BranchOp op;
  uint32_t lhs = 0;
  uint32_t rhs = 0;
  bool taken = false;
};

struct LoadOp {
  const char *name = "";
  uint8_t funct3 = 0;
};

struct LoadVector {
  std::string name;
  LoadOp op;
  std::vector<uint8_t> memory;
  uint32_t address = 0;
  int32_t offset = 0;
  uint32_t expected = 0;
};

struct StoreOp {
  const char *name = "";
  uint8_t funct3 = 0;
};

struct StoreVector {
  std::string name;
  StoreOp op;
  uint32_t value = 0;
  uint32_t address = 0;
  int32_t offset = 0;
  std::vector<uint8_t> expected_bytes;
};

struct UpperVector {
  std::string name;
  uint32_t opcode = 0;
  uint32_t imm = 0;
  uint32_t initial_pc = 0;
  uint32_t expected = 0;
};

struct JumpVector {
  std::string name;
  uint32_t first_instruction = 0;
  uint32_t initial_x1 = 0;
  uint8_t link_reg = 0;
  uint32_t expected_link = 0;
  uint32_t expected_marker = 0;
};

void
expect_halts(Machine &machine)
{
  HaltHost host;
  ASSERT_EQ(machine.run(host), StepResult::kHalted);
  EXPECT_TRUE(machine.halted());
  EXPECT_EQ(machine.trap(), Trap::kNone);
}

uint32_t
base_for(uint32_t target_address, int32_t offset)
{
  return target_address - static_cast<uint32_t>(offset);
}

void
run_rr_vector(const RrVector &test_case, uint8_t rd)
{
  const auto program = make_program(
      {r_type(test_case.op.funct7, 2, 1, test_case.op.funct3, rd), kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 256, 1000);
  MachineTestPeer::set_reg(machine, 1, test_case.lhs);
  MachineTestPeer::set_reg(machine, 2, test_case.rhs);

  expect_halts(machine);
  EXPECT_EQ(machine.reg(rd), rd == 0 ? 0U : test_case.expected)
      << test_case.name << " rd=x" << unsigned{rd};
}

void
run_load_vector(const LoadVector &test_case, uint8_t rd)
{
  const auto program = make_program(
      {load(rd, test_case.op.funct3, 1, test_case.offset), kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 256, 1000);
  ASSERT_TRUE(machine.view().write_memory(kDataAddress, test_case.memory));
  MachineTestPeer::set_reg(machine, 1,
                           base_for(test_case.address, test_case.offset));

  expect_halts(machine);
  EXPECT_EQ(machine.reg(rd), rd == 0 ? 0U : test_case.expected)
      << test_case.name << " rd=x" << unsigned{rd};
}

void
run_store_vector(const StoreVector &test_case)
{
  const auto program = make_program(
      {s_type(test_case.op.funct3, 1, 2, test_case.offset), kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 256, 1000);
  std::vector<uint8_t> sentinels(128, kSentinelByte);
  ASSERT_TRUE(machine.view().write_memory(kDataAddress - 8, sentinels));
  MachineTestPeer::set_reg(machine, 1,
                           base_for(test_case.address, test_case.offset));
  MachineTestPeer::set_reg(machine, 2, test_case.value);

  expect_halts(machine);
  ASSERT_TRUE(memory_range_in_bounds(machine.memory().size(), test_case.address,
                                     test_case.expected_bytes.size()))
      << test_case.name;
  const auto actual = machine.memory().subspan(test_case.address,
                                               test_case.expected_bytes.size());
  EXPECT_EQ(std::vector<uint8_t>(actual.begin(), actual.end()),
            test_case.expected_bytes)
      << test_case.name;
  const auto sentinel_region =
      machine.memory().subspan(kDataAddress - 8, sentinels.size());
  const uint32_t clobber_end =
      test_case.address +
      static_cast<uint32_t>(test_case.expected_bytes.size());
  for (size_t i = 0; i < sentinel_region.size(); ++i) {
    const uint32_t address = (kDataAddress - 8) + static_cast<uint32_t>(i);
    if (address >= test_case.address && address < clobber_end) {
      continue;
    }
    EXPECT_EQ(sentinel_region[i], kSentinelByte)
        << test_case.name << " clobbered memory@" << address;
  }
}

void
run_forward_branch_vector(const BranchVector &test_case)
{
  const auto program =
      make_program({test_program::b_type(test_case.op.funct3, 1, 2, 12),
                    test_program::addi(3, 0, 1), kEcall,
                    test_program::addi(3, 0, 2), kEcall});
  ASSERT_NE(program, nullptr);

  Machine machine(program, 256, 1000);
  MachineTestPeer::set_reg(machine, 1, test_case.lhs);
  MachineTestPeer::set_reg(machine, 2, test_case.rhs);

  expect_halts(machine);
  EXPECT_EQ(machine.reg(3), test_case.taken ? 2U : 1U)
      << test_case.name << " forward";
}

void
run_backward_branch_vector(const BranchVector &test_case)
{
  const auto program =
      make_program({test_program::addi(3, 0, 2), kEcall,
                    test_program::b_type(test_case.op.funct3, 1, 2, -8),
                    test_program::addi(3, 0, 1), kEcall});
  ASSERT_NE(program, nullptr);

  Machine machine(program, 256, 1000, 8);
  MachineTestPeer::set_reg(machine, 1, test_case.lhs);
  MachineTestPeer::set_reg(machine, 2, test_case.rhs);

  expect_halts(machine);
  EXPECT_EQ(machine.reg(3), test_case.taken ? 2U : 1U)
      << test_case.name << " backward";
}

void
run_imm_vector(const ImmVector &test_case, uint8_t rd)
{
  const auto program =
      make_program({i_type(0x13U, rd, test_case.op.funct3, 1,
                           static_cast<int32_t>(test_case.op.imm_bits)),
                    kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 256, 1000);
  MachineTestPeer::set_reg(machine, 1, test_case.lhs);

  expect_halts(machine);
  EXPECT_EQ(machine.reg(rd), rd == 0 ? 0U : test_case.expected)
      << test_case.name << " rd=x" << unsigned{rd};
}

std::vector<RrVector>
rr_vectors()
{
  // Sources: riscv-tests
  // tests/rv32ui/{add,sub,and,or,xor,slt,sltu,sll,srl,sra}.S and
  // tests/rv32um/{mul,mulh,mulhsu,mulhu,div,divu,rem,remu}.S.
  constexpr RrOp kAdd{"add", 0x00, 0};
  constexpr RrOp kSub{"sub", 0x20, 0};
  constexpr RrOp kSll{"sll", 0x00, 1};
  constexpr RrOp kSlt{"slt", 0x00, 2};
  constexpr RrOp kSltu{"sltu", 0x00, 3};
  constexpr RrOp kXor{"xor", 0x00, 4};
  constexpr RrOp kSrl{"srl", 0x00, 5};
  constexpr RrOp kSra{"sra", 0x20, 5};
  constexpr RrOp kOr{"or", 0x00, 6};
  constexpr RrOp kAnd{"and", 0x00, 7};
  constexpr RrOp kMul{"mul", 0x01, 0};
  constexpr RrOp kMulh{"mulh", 0x01, 1};
  constexpr RrOp kMulhsu{"mulhsu", 0x01, 2};
  constexpr RrOp kMulhu{"mulhu", 0x01, 3};
  constexpr RrOp kDiv{"div", 0x01, 4};
  constexpr RrOp kDivu{"divu", 0x01, 5};
  constexpr RrOp kRem{"rem", 0x01, 6};
  constexpr RrOp kRemu{"remu", 0x01, 7};

  return {
      {"add_wrap_positive", kAdd, 0x7fffffffU, 0x00007fffU, 0x80007ffeU},
      {"add_negative_plus_one", kAdd, 0xffffffffU, 0x00000001U, 0x00000000U},
      {"sub_underflow", kSub, 0x00000000U, 0x00007fffU, 0xffff8001U},
      {"sub_min_minus_small", kSub, 0x80000000U, 0x00007fffU, 0x7fff8001U},
      {"and_checkerboard", kAnd, 0xff00ff00U, 0x0f0f0f0fU, 0x0f000f00U},
      {"or_checkerboard", kOr, 0x0ff00ff0U, 0xf0f0f0f0U, 0xfff0fff0U},
      {"xor_checkerboard", kXor, 0x00ff00ffU, 0x0f0f0f0fU, 0x0ff00ff0U},
      {"slt_min_less_zero", kSlt, 0x80000000U, 0x00000000U, 0x00000001U},
      {"slt_unsigned_max_not_less_one", kSltu, 0xffffffffU, 0x00000001U,
       0x00000000U},
      {"sll_masks_shift_amount", kSll, 0x21212121U, 0xffffffffU, 0x80000000U},
      {"srl_masks_shift_amount", kSrl, 0x21212121U, 0xffffffffU, 0x00000000U},
      {"sra_negative_fill", kSra, 0x81818181U, 0x0000001fU, 0xffffffffU},
      {"mul_low_wrap", kMul, 0xaaaaaaabU, 0x0002fe7dU, 0x0000ff7fU},
      {"mulh_signed_high", kMulh, 0xaaaaaaabU, 0x0002fe7dU, 0xffff0081U},
      {"mulhsu_mixed_high", kMulhsu, 0x80000000U, 0xffff8000U, 0x80004000U},
      {"mulhu_unsigned_high", kMulhu, 0xff000000U, 0xff000000U, 0xfe010000U},
      {"div_signed_negative", kDiv, 0xffffffecU, 0x00000006U, 0xfffffffdU},
      {"div_signed_overflow", kDiv, 0x80000000U, 0xffffffffU, 0x80000000U},
      {"div_signed_by_zero", kDiv, 0x00000001U, 0x00000000U, 0xffffffffU},
      {"divu_large", kDivu, 0xffffffecU, 0x00000006U, 0x2aaaaaa7U},
      {"rem_signed_negative", kRem, 0xffffffecU, 0x00000006U, 0xfffffffeU},
      {"rem_signed_by_zero", kRem, 0x80000000U, 0x00000000U, 0x80000000U},
      {"remu_large", kRemu, 0xffffffecU, 0xfffffffaU, 0xffffffecU},
  };
}

std::vector<ImmVector>
imm_vectors()
{
  // Sources: riscv-tests tests/rv32ui/{addi,slti,sltiu,xori,ori,andi,slli,
  // srli,srai}.S.
  return {
      {"addi_sign_extreme", {"addi", 0, 0x800}, 0x80000000U, 0x7ffff800U},
      {"addi_wrap", {"addi", 0, 0x001}, 0x7fffffffU, 0x80000000U},
      {"slti_min_less_zero", {"slti", 2, 0x000}, 0x80000000U, 1U},
      {"sltiu_zero_less_negative_imm", {"sltiu", 3, 0x800}, 0x00000000U, 1U},
      {"xori_sign_extended_imm", {"xori", 4, 0xf0f}, 0x00ff0f00U, 0xff00f00fU},
      {"ori_sign_extended_imm", {"ori", 6, 0xf0f}, 0xff00ff00U, 0xffffff0fU},
      {"andi_sign_extended_imm", {"andi", 7, 0xf0f}, 0xff00ff00U, 0xff00ff00U},
      {"slli_top_bit", {"slli", 1, 31}, 0x00000001U, 0x80000000U},
      {"srli_top_bit", {"srli", 5, 31}, 0x80000001U, 0x00000001U},
      {"srai_top_bit", {"srai", 5, 0x400 | 31}, 0x80000001U, 0xffffffffU},
  };
}

std::vector<BranchVector>
branch_vectors()
{
  // Sources: riscv-tests tests/rv32ui/{beq,bne,blt,bge,bltu,bgeu}.S
  // TEST_BR2_OP_TAKEN/NOTTAKEN operand matrices.
  constexpr BranchOp kBeq{"beq", 0};
  constexpr BranchOp kBne{"bne", 1};
  constexpr BranchOp kBlt{"blt", 4};
  constexpr BranchOp kBge{"bge", 5};
  constexpr BranchOp kBltu{"bltu", 6};
  constexpr BranchOp kBgeu{"bgeu", 7};

  return {
      // Full taken/not-taken operand matrices from the riscv-tests branch
      // files. The original tests also cover bypass timing, which is not
      // meaningful for this non-pipelined interpreter.
      {"beq_taken", kBeq, 0x00000001U, 0x00000001U, true},
      {"beq_zero_taken", kBeq, 0x00000000U, 0x00000000U, true},
      {"beq_negative_taken", kBeq, 0xffffffffU, 0xffffffffU, true},
      {"beq_not_taken", kBeq, 0x00000001U, 0x00000002U, false},
      {"beq_zero_one_not_taken", kBeq, 0x00000000U, 0x00000001U, false},
      {"beq_negative_positive_not_taken", kBeq, 0xffffffffU, 0x00000001U,
       false},
      {"beq_positive_negative_not_taken", kBeq, 0x00000001U, 0xffffffffU,
       false},
      {"bne_taken", kBne, 0x00000001U, 0x00000002U, true},
      {"bne_zero_one_taken", kBne, 0x00000000U, 0x00000001U, true},
      {"bne_one_zero_taken", kBne, 0x00000001U, 0x00000000U, true},
      {"bne_negative_positive_taken", kBne, 0xffffffffU, 0x00000001U, true},
      {"bne_positive_negative_taken", kBne, 0x00000001U, 0xffffffffU, true},
      {"bne_not_taken", kBne, 0x00000001U, 0x00000001U, false},
      {"bne_zero_not_taken", kBne, 0x00000000U, 0x00000000U, false},
      {"bne_negative_not_taken", kBne, 0xffffffffU, 0xffffffffU, false},
      {"blt_signed_min_taken", kBlt, 0x80000000U, 0x00000000U, true},
      {"blt_zero_one_taken", kBlt, 0x00000000U, 0x00000001U, true},
      {"blt_negative_one_taken", kBlt, 0xffffffffU, 0x00000001U, true},
      {"blt_negative_order_taken", kBlt, 0xfffffffeU, 0xffffffffU, true},
      {"blt_signed_positive_not_taken", kBlt, 0x00000007U, 0x00000003U, false},
      {"blt_one_zero_not_taken", kBlt, 0x00000001U, 0x00000000U, false},
      {"blt_positive_negative_not_taken", kBlt, 0x00000001U, 0xffffffffU,
       false},
      {"blt_negative_order_not_taken", kBlt, 0xffffffffU, 0xfffffffeU, false},
      {"blt_one_minus_two_not_taken", kBlt, 0x00000001U, 0xfffffffeU, false},
      {"bge_signed_negative_not_taken", kBge, 0xffffffffU, 0x00000001U, false},
      {"bge_signed_equal_taken", kBge, 0xffffffffU, 0xffffffffU, true},
      {"bge_zero_equal_taken", kBge, 0x00000000U, 0x00000000U, true},
      {"bge_one_equal_taken", kBge, 0x00000001U, 0x00000001U, true},
      {"bge_one_zero_taken", kBge, 0x00000001U, 0x00000000U, true},
      {"bge_one_negative_taken", kBge, 0x00000001U, 0xffffffffU, true},
      {"bge_negative_order_taken", kBge, 0xffffffffU, 0xfffffffeU, true},
      {"bge_zero_one_not_taken", kBge, 0x00000000U, 0x00000001U, false},
      {"bge_negative_order_not_taken", kBge, 0xfffffffeU, 0xffffffffU, false},
      {"bge_minus_two_one_not_taken", kBge, 0xfffffffeU, 0x00000001U, false},
      {"bltu_zero_less_max_taken", kBltu, 0x00000000U, 0xffffffffU, true},
      {"bltu_zero_one_taken", kBltu, 0x00000000U, 0x00000001U, true},
      {"bltu_max_minus_one_taken", kBltu, 0xfffffffeU, 0xffffffffU, true},
      {"bltu_max_less_one_not_taken", kBltu, 0xffffffffU, 0x00000001U, false},
      {"bltu_one_zero_not_taken", kBltu, 0x00000001U, 0x00000000U, false},
      {"bltu_max_order_not_taken", kBltu, 0xffffffffU, 0xfffffffeU, false},
      {"bltu_max_zero_not_taken", kBltu, 0xffffffffU, 0x00000000U, false},
      {"bltu_high_bit_not_taken", kBltu, 0x80000000U, 0x7fffffffU, false},
      {"bgeu_max_greater_one_taken", kBgeu, 0xffffffffU, 0x00000001U, true},
      {"bgeu_zero_equal_taken", kBgeu, 0x00000000U, 0x00000000U, true},
      {"bgeu_one_equal_taken", kBgeu, 0x00000001U, 0x00000001U, true},
      {"bgeu_max_equal_taken", kBgeu, 0xffffffffU, 0xffffffffU, true},
      {"bgeu_one_zero_taken", kBgeu, 0x00000001U, 0x00000000U, true},
      {"bgeu_max_order_taken", kBgeu, 0xffffffffU, 0xfffffffeU, true},
      {"bgeu_max_zero_taken", kBgeu, 0xffffffffU, 0x00000000U, true},
      {"bgeu_zero_greater_max_not_taken", kBgeu, 0x00000000U, 0xffffffffU,
       false},
      {"bgeu_zero_one_not_taken", kBgeu, 0x00000000U, 0x00000001U, false},
      {"bgeu_max_minus_one_not_taken", kBgeu, 0xfffffffeU, 0xffffffffU, false},
      {"bgeu_high_bit_not_taken", kBgeu, 0x7fffffffU, 0x80000000U, false},
  };
}

std::vector<LoadVector>
load_vectors()
{
  // Sources: riscv-tests tests/rv32ui/{lb,lbu,lh,lhu,lw}.S TEST_LD_OP basic
  // and negative-offset cases.
  constexpr LoadOp kLb{"lb", 0};
  constexpr LoadOp kLh{"lh", 1};
  constexpr LoadOp kLw{"lw", 2};
  constexpr LoadOp kLbu{"lbu", 4};
  constexpr LoadOp kLhu{"lhu", 5};

  return {
      {"lb_byte_ff",
       kLb,
       {0xff, 0x00, 0xf0, 0x0f},
       kDataAddress,
       0,
       0xffffffffU},
      {"lb_byte_zero", kLb, {0xff, 0x00, 0xf0, 0x0f}, kDataAddress + 1, 1, 0},
      {"lb_byte_f0",
       kLb,
       {0xff, 0x00, 0xf0, 0x0f},
       kDataAddress + 2,
       2,
       0xfffffff0U},
      {"lb_negative_offset",
       kLb,
       {0xff, 0x00, 0xf0, 0x0f},
       kDataAddress,
       -3,
       0xffffffffU},
      {"lbu_byte_ff", kLbu, {0xff, 0x00, 0xf0, 0x0f}, kDataAddress, 0, 0xffU},
      {"lbu_byte_f0",
       kLbu,
       {0xff, 0x00, 0xf0, 0x0f},
       kDataAddress + 2,
       2,
       0xf0U},
      {"lbu_negative_offset",
       kLbu,
       {0xff, 0x00, 0xf0, 0x0f},
       kDataAddress,
       -3,
       0xffU},
      {"lh_positive",
       kLh,
       {0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0x0f, 0xf0},
       kDataAddress,
       0,
       0x000000ffU},
      {"lh_negative",
       kLh,
       {0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0x0f, 0xf0},
       kDataAddress + 2,
       2,
       0xffffff00U},
      {"lh_negative_offset",
       kLh,
       {0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0x0f, 0xf0},
       kDataAddress + 4,
       -2,
       0x00000ff0U},
      {"lhu_positive",
       kLhu,
       {0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0x0f, 0xf0},
       kDataAddress,
       0,
       0x000000ffU},
      {"lhu_unsigned",
       kLhu,
       {0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0x0f, 0xf0},
       kDataAddress + 2,
       2,
       0x0000ff00U},
      {"lhu_negative_offset",
       kLhu,
       {0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0x0f, 0xf0},
       kDataAddress + 4,
       -2,
       0x00000ff0U},
      {"lw_positive",
       kLw,
       {0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xf0, 0x0f, 0xf0, 0x0f,
        0x0f, 0xf0, 0x0f, 0xf0},
       kDataAddress,
       0,
       0x00ff00ffU},
      {"lw_negative",
       kLw,
       {0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xf0, 0x0f, 0xf0, 0x0f,
        0x0f, 0xf0, 0x0f, 0xf0},
       kDataAddress + 4,
       4,
       0xff00ff00U},
      {"lw_negative_offset",
       kLw,
       {0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xf0, 0x0f, 0xf0, 0x0f,
        0x0f, 0xf0, 0x0f, 0xf0},
       kDataAddress + 8,
       -4,
       0x0ff00ff0U},
  };
}

std::vector<StoreVector>
store_vectors()
{
  // Sources: riscv-tests tests/rv32ui/{sb,sh,sw}.S TEST_ST_OP basic and
  // negative-offset cases.
  constexpr StoreOp kSb{"sb", 0};
  constexpr StoreOp kSh{"sh", 1};
  constexpr StoreOp kSw{"sw", 2};

  return {
      {"sb_low_byte", kSb, 0x123456aaU, kDataAddress, 0, {0xaa}},
      {"sb_zero_byte", kSb, 0, kDataAddress + 1, 1, {0}},
      {"sb_negative_offset", kSb, 0x123456aaU, kDataAddress, -3, {0xaa}},
      {"sh_low_half", kSh, 0x123400aaU, kDataAddress, 0, {0xaa, 0x00}},
      {"sh_high_low_half", kSh, 0x1234aa00U, kDataAddress + 2, 2, {0x00, 0xaa}},
      {"sh_negative_offset",
       kSh,
       0x12340aa0U,
       kDataAddress + 4,
       -2,
       {0xa0, 0x0a}},
      {"sw_low_word",
       kSw,
       0x00aa00aaU,
       kDataAddress,
       0,
       {0xaa, 0x00, 0xaa, 0x00}},
      {"sw_high_word",
       kSw,
       0xaa00aa00U,
       kDataAddress + 4,
       4,
       {0x00, 0xaa, 0x00, 0xaa}},
      {"sw_negative_offset",
       kSw,
       0x0aa00aa0U,
       kDataAddress + 8,
       -4,
       {0xa0, 0x0a, 0xa0, 0x0a}},
  };
}

std::vector<UpperVector>
upper_vectors()
{
  // Sources: riscv-tests tests/rv32ui/{lui,auipc}.S basic cases, adapted to
  // direct PC-relative expectations.
  return {
      {"lui_zero", 0x37U, 0x00000000U, 0, 0x00000000U},
      {"lui_negative", 0x37U, 0xfffff000U, 0, 0xfffff000U},
      {"lui_sign_bit", 0x37U, 0x80000000U, 0, 0x80000000U},
      {"auipc_positive", 0x17U, 0x00002000U, 4, 0x00002004U},
      {"auipc_negative", 0x17U, 0xffffe000U, 8, 0xffffe008U},
  };
}

std::vector<JumpVector>
jump_vectors()
{
  // Sources: riscv-tests tests/rv32ui/{jal,jalr}.S basic, x0 link discard,
  // rd==rs1, and low-bit masking cases.
  return {
      {"jal_skips_delay_slot_like_code", j_type(4, 12), 0, 4, 4, 0x33},
      {"jal_x0_discards_link", j_type(0, 12), 0, 0, 0, 0x33},
      {"jalr_basic", test_program::jalr(4, 1, 0), 12, 4, 4, 0x33},
      {"jalr_masks_low_target_bit", test_program::jalr(4, 1, 1), 11, 4, 4,
       0x33},
      {"jalr_rd_equals_rs1", test_program::jalr(1, 1, 0), 12, 1, 4, 0x33},
  };
}

std::string
rr_name(const testing::TestParamInfo<RrVector> &info)
{
  return info.param.name;
}

std::string
imm_name(const testing::TestParamInfo<ImmVector> &info)
{
  return info.param.name;
}

std::string
branch_name(const testing::TestParamInfo<BranchVector> &info)
{
  return info.param.name;
}

std::string
load_name(const testing::TestParamInfo<LoadVector> &info)
{
  return info.param.name;
}

std::string
store_name(const testing::TestParamInfo<StoreVector> &info)
{
  return info.param.name;
}

std::string
upper_name(const testing::TestParamInfo<UpperVector> &info)
{
  return info.param.name;
}

std::string
jump_name(const testing::TestParamInfo<JumpVector> &info)
{
  return info.param.name;
}

class BorrowedRrConformanceTest : public testing::TestWithParam<RrVector> {};
class BorrowedImmConformanceTest : public testing::TestWithParam<ImmVector> {};
class BorrowedBranchConformanceTest
  : public testing::TestWithParam<BranchVector> {};
class BorrowedLoadConformanceTest : public testing::TestWithParam<LoadVector> {
};
class BorrowedStoreConformanceTest
  : public testing::TestWithParam<StoreVector> {};
class BorrowedUpperConformanceTest
  : public testing::TestWithParam<UpperVector> {};
class BorrowedJumpConformanceTest : public testing::TestWithParam<JumpVector> {
};

TEST_P(BorrowedRrConformanceTest, matchesRiscvTestsVector)
{
  const RrVector &test_case = GetParam();

  run_rr_vector(test_case, 3);
  run_rr_vector(test_case, 1);
  run_rr_vector(test_case, 2);
  run_rr_vector(test_case, 0);
}

TEST_P(BorrowedImmConformanceTest, matchesRiscvTestsVector)
{
  const ImmVector &test_case = GetParam();

  run_imm_vector(test_case, 3);
  run_imm_vector(test_case, 1);
  run_imm_vector(test_case, 0);
}

TEST_P(BorrowedBranchConformanceTest, matchesRiscvTestsVector)
{
  const BranchVector &test_case = GetParam();

  run_forward_branch_vector(test_case);
  run_backward_branch_vector(test_case);
}

TEST_P(BorrowedLoadConformanceTest, matchesRiscvTestsVector)
{
  const LoadVector &test_case = GetParam();

  run_load_vector(test_case, 3);
  run_load_vector(test_case, 1);
  run_load_vector(test_case, 0);
}

TEST_P(BorrowedStoreConformanceTest, matchesRiscvTestsVector)
{
  run_store_vector(GetParam());
}

TEST_P(BorrowedUpperConformanceTest, matchesRiscvTestsVector)
{
  const UpperVector &test_case = GetParam();
  std::vector<uint32_t> words(test_case.initial_pc / kInstructionBytes,
                              test_program::addi(0, 0, 0));
  words.push_back(u_type(test_case.opcode, 3, test_case.imm));
  words.push_back(kEcall);

  const auto program =
      make_program(std::span<const uint32_t>(words.data(), words.size()));
  ASSERT_NE(program, nullptr);
  Machine machine(program, 256, 1000, test_case.initial_pc);

  expect_halts(machine);
  EXPECT_EQ(machine.reg(3), test_case.expected) << test_case.name;
}

TEST(BorrowedUpperConformanceTest, luiToX0KeepsZero)
{
  const auto program = make_program({u_type(0x37U, 0, 0x80000000U), kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 256, 1000);

  expect_halts(machine);
  EXPECT_EQ(machine.reg(0), 0U);
}

TEST_P(BorrowedJumpConformanceTest, matchesRiscvTestsVector)
{
  const JumpVector &test_case = GetParam();
  const auto program =
      make_program({test_case.first_instruction, test_program::addi(5, 0, 0x11),
                    kEcall, test_program::addi(5, 0, 0x33), kEcall});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 256, 1000);
  MachineTestPeer::set_reg(machine, 1, test_case.initial_x1);

  expect_halts(machine);
  EXPECT_EQ(machine.reg(test_case.link_reg), test_case.expected_link)
      << test_case.name;
  EXPECT_EQ(machine.reg(5), test_case.expected_marker) << test_case.name;
}

INSTANTIATE_TEST_SUITE_P(RiscvTests, BorrowedRrConformanceTest,
                         testing::ValuesIn(rr_vectors()), rr_name);
INSTANTIATE_TEST_SUITE_P(RiscvTests, BorrowedImmConformanceTest,
                         testing::ValuesIn(imm_vectors()), imm_name);
INSTANTIATE_TEST_SUITE_P(RiscvTests, BorrowedBranchConformanceTest,
                         testing::ValuesIn(branch_vectors()), branch_name);
INSTANTIATE_TEST_SUITE_P(RiscvTests, BorrowedLoadConformanceTest,
                         testing::ValuesIn(load_vectors()), load_name);
INSTANTIATE_TEST_SUITE_P(RiscvTests, BorrowedStoreConformanceTest,
                         testing::ValuesIn(store_vectors()), store_name);
INSTANTIATE_TEST_SUITE_P(RiscvTests, BorrowedUpperConformanceTest,
                         testing::ValuesIn(upper_vectors()), upper_name);
INSTANTIATE_TEST_SUITE_P(RiscvTests, BorrowedJumpConformanceTest,
                         testing::ValuesIn(jump_vectors()), jump_name);

} // namespace
} // namespace hogd::rv32

// NOLINTEND(google-readability-avoid-underscore-in-googletest-name,modernize-use-designated-initializers)
