#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hogd::rv32 {

constexpr uint32_t kInstructionBytes = 4;
constexpr uint32_t kRegisterCount = 32;
constexpr uint32_t kMaxMemoryBytes = 64 * 1024;
constexpr uint32_t kMaxCodeBytes = 64 * 1024;
constexpr uint32_t kGasTableVersion = 1;
constexpr uint64_t kBaseInstructionGas = 1;
constexpr uint64_t kMulInstructionGas = 3;
constexpr uint64_t kDivInstructionGas = 8;

enum class ProgramError {
  kNone,
  kCodeSizeNotAligned,
  kCodeTooLarge,
  kUnsupportedCompressedInstruction,
  kUnsupportedSystemInstruction,
  kUnsupportedInstruction,
};

enum class InstructionKind {
  kLui,
  kAuipc,
  kJal,
  kJalr,
  kBranch,
  kLoad,
  kStore,
  kOpImm,
  kOp,
  kFence,
  kEcall,
};

enum class Op {
  kLui,
  kAuipc,
  kJal,
  kJalr,
  kBeq,
  kBne,
  kBlt,
  kBge,
  kBltu,
  kBgeu,
  kLb,
  kLh,
  kLw,
  kLbu,
  kLhu,
  kSb,
  kSh,
  kSw,
  kAddi,
  kSlti,
  kSltiu,
  kXori,
  kOri,
  kAndi,
  kSlli,
  kSrli,
  kSrai,
  kAdd,
  kSub,
  kSll,
  kSlt,
  kSltu,
  kXor,
  kSrl,
  kSra,
  kOr,
  kAnd,
  kMul,
  kMulh,
  kMulhsu,
  kMulhu,
  kDiv,
  kDivu,
  kRem,
  kRemu,
  kFence,
  kEcall,
  kCount,
};

constexpr size_t kOpCount = static_cast<size_t>(Op::kCount);

constexpr std::array<uint64_t, kOpCount>
make_instruction_gas_costs()
{
  std::array<uint64_t, kOpCount> costs = {};
  costs.fill(kBaseInstructionGas);
  costs[static_cast<size_t>(Op::kMul)] = kMulInstructionGas;
  costs[static_cast<size_t>(Op::kMulh)] = kMulInstructionGas;
  costs[static_cast<size_t>(Op::kMulhsu)] = kMulInstructionGas;
  costs[static_cast<size_t>(Op::kMulhu)] = kMulInstructionGas;
  costs[static_cast<size_t>(Op::kDiv)] = kDivInstructionGas;
  costs[static_cast<size_t>(Op::kDivu)] = kDivInstructionGas;
  costs[static_cast<size_t>(Op::kRem)] = kDivInstructionGas;
  costs[static_cast<size_t>(Op::kRemu)] = kDivInstructionGas;
  return costs;
}

constexpr std::array<uint64_t, kOpCount> kInstructionGasCosts =
    make_instruction_gas_costs();

constexpr bool
gas_costs_are_positive()
{
  for (const uint64_t cost : kInstructionGasCosts) {
    if (cost == 0) {
      return false;
    }
  }
  return true;
}

static_assert(gas_costs_are_positive());

constexpr uint64_t
gas_cost_for_op(Op op)
{
  return kInstructionGasCosts[static_cast<size_t>(op)];
}

struct DecodedInstruction {
  uint32_t word = 0;
  InstructionKind kind = InstructionKind::kOp;
  Op op = Op::kAdd;
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  uint8_t funct3 = 0;
  uint8_t funct7 = 0;
  int32_t immediate = 0;
  uint64_t gas_cost = kBaseInstructionGas;
};

struct DecodeResult {
  bool ok = false;
  ProgramError error = ProgramError::kNone;
  DecodedInstruction instruction = {};
};

DecodeResult decode_instruction(uint32_t word);

struct ProgramResult {
  bool ok = false;
  ProgramError error = ProgramError::kNone;
};

class Program {
public:
  static ProgramResult validate(std::span<const uint8_t> code);
  static ProgramResult create(std::span<const uint8_t> code, Program &out);

  Program() = default;

  [[nodiscard]] size_t instruction_count() const
  {
    return instructions_.size();
  }
  [[nodiscard]] size_t byte_size() const
  {
    return instructions_.size() * kInstructionBytes;
  }
  [[nodiscard]] bool contains_pc(uint32_t pc) const;
  [[nodiscard]] const DecodedInstruction &
  instruction_at_aligned_pc(uint32_t pc) const;

private:
  std::vector<DecodedInstruction> instructions_;
};

} // namespace hogd::rv32
