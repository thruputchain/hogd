#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace hogd::rv32 {

constexpr uint32_t kInstructionBytes = 4;
constexpr uint32_t kRegisterCount = 32;
constexpr uint32_t kMaxMemoryBytes = 64 * 1024;
constexpr uint32_t kMaxCodeBytes = 64 * 1024;
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

struct InstructionGasEntry {
  Op op;
  const char *name;
  uint64_t cost;
};

constexpr std::array<InstructionGasEntry, kOpCount> kInstructionGasSchedule = {{
    {.op = Op::kLui, .name = "LUI", .cost = kBaseInstructionGas},
    {.op = Op::kAuipc, .name = "AUIPC", .cost = kBaseInstructionGas},
    {.op = Op::kJal, .name = "JAL", .cost = kBaseInstructionGas},
    {.op = Op::kJalr, .name = "JALR", .cost = kBaseInstructionGas},
    {.op = Op::kBeq, .name = "BEQ", .cost = kBaseInstructionGas},
    {.op = Op::kBne, .name = "BNE", .cost = kBaseInstructionGas},
    {.op = Op::kBlt, .name = "BLT", .cost = kBaseInstructionGas},
    {.op = Op::kBge, .name = "BGE", .cost = kBaseInstructionGas},
    {.op = Op::kBltu, .name = "BLTU", .cost = kBaseInstructionGas},
    {.op = Op::kBgeu, .name = "BGEU", .cost = kBaseInstructionGas},
    {.op = Op::kLb, .name = "LB", .cost = kBaseInstructionGas},
    {.op = Op::kLh, .name = "LH", .cost = kBaseInstructionGas},
    {.op = Op::kLw, .name = "LW", .cost = kBaseInstructionGas},
    {.op = Op::kLbu, .name = "LBU", .cost = kBaseInstructionGas},
    {.op = Op::kLhu, .name = "LHU", .cost = kBaseInstructionGas},
    {.op = Op::kSb, .name = "SB", .cost = kBaseInstructionGas},
    {.op = Op::kSh, .name = "SH", .cost = kBaseInstructionGas},
    {.op = Op::kSw, .name = "SW", .cost = kBaseInstructionGas},
    {.op = Op::kAddi, .name = "ADDI", .cost = kBaseInstructionGas},
    {.op = Op::kSlti, .name = "SLTI", .cost = kBaseInstructionGas},
    {.op = Op::kSltiu, .name = "SLTIU", .cost = kBaseInstructionGas},
    {.op = Op::kXori, .name = "XORI", .cost = kBaseInstructionGas},
    {.op = Op::kOri, .name = "ORI", .cost = kBaseInstructionGas},
    {.op = Op::kAndi, .name = "ANDI", .cost = kBaseInstructionGas},
    {.op = Op::kSlli, .name = "SLLI", .cost = kBaseInstructionGas},
    {.op = Op::kSrli, .name = "SRLI", .cost = kBaseInstructionGas},
    {.op = Op::kSrai, .name = "SRAI", .cost = kBaseInstructionGas},
    {.op = Op::kAdd, .name = "ADD", .cost = kBaseInstructionGas},
    {.op = Op::kSub, .name = "SUB", .cost = kBaseInstructionGas},
    {.op = Op::kSll, .name = "SLL", .cost = kBaseInstructionGas},
    {.op = Op::kSlt, .name = "SLT", .cost = kBaseInstructionGas},
    {.op = Op::kSltu, .name = "SLTU", .cost = kBaseInstructionGas},
    {.op = Op::kXor, .name = "XOR", .cost = kBaseInstructionGas},
    {.op = Op::kSrl, .name = "SRL", .cost = kBaseInstructionGas},
    {.op = Op::kSra, .name = "SRA", .cost = kBaseInstructionGas},
    {.op = Op::kOr, .name = "OR", .cost = kBaseInstructionGas},
    {.op = Op::kAnd, .name = "AND", .cost = kBaseInstructionGas},
    {.op = Op::kMul, .name = "MUL", .cost = kMulInstructionGas},
    {.op = Op::kMulh, .name = "MULH", .cost = kMulInstructionGas},
    {.op = Op::kMulhsu, .name = "MULHSU", .cost = kMulInstructionGas},
    {.op = Op::kMulhu, .name = "MULHU", .cost = kMulInstructionGas},
    {.op = Op::kDiv, .name = "DIV", .cost = kDivInstructionGas},
    {.op = Op::kDivu, .name = "DIVU", .cost = kDivInstructionGas},
    {.op = Op::kRem, .name = "REM", .cost = kDivInstructionGas},
    {.op = Op::kRemu, .name = "REMU", .cost = kDivInstructionGas},
    {.op = Op::kFence, .name = "FENCE", .cost = kBaseInstructionGas},
    {.op = Op::kEcall, .name = "ECALL", .cost = kBaseInstructionGas},
}};

constexpr bool
gas_schedule_shape_is_valid()
{
  for (size_t i = 0; i < kInstructionGasSchedule.size(); ++i) {
    if (static_cast<size_t>(kInstructionGasSchedule[i].op) != i ||
        kInstructionGasSchedule[i].cost == 0) {
      return false;
    }
  }
  return true;
}

static_assert(gas_schedule_shape_is_valid());

constexpr std::array<uint64_t, kOpCount>
make_instruction_gas_costs()
{
  std::array<uint64_t, kOpCount> costs = {};
  for (const auto &entry : kInstructionGasSchedule) {
    costs[static_cast<size_t>(entry.op)] = entry.cost;
  }
  return costs;
}

constexpr std::array<uint64_t, kOpCount> kInstructionGasCosts =
    make_instruction_gas_costs();

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

// Immutable, already-decoded RV32IM code. Program is the validation boundary:
// Machine never executes raw bytes and never lets guest memory alias code.
class Program {
public:
  static ProgramResult validate(std::span<const uint8_t> code);
  static std::expected<Program, ProgramError>
  create(std::span<const uint8_t> code);

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
  Program() = default;

  std::vector<DecodedInstruction> instructions_;
};

} // namespace hogd::rv32
