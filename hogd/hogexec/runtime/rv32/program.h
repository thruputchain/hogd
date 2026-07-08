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
  kMemoryTooLarge,
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
  kEbreak,
  kUnimp,
};

enum class Op : uint8_t {
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
  kEbreak,
  kUnimp,
  kCount,
};

constexpr size_t kOpCount = static_cast<size_t>(Op::kCount);

struct InstructionGasEntry {
  Op op;
  // `u8"..."` literal: folded into the RV32 gas-schedule digest (an artifact
  // wire constant), so the hashed bytes must be the standard's UTF-8 code units
  // rather than implementation-defined ordinary-literal bytes that depend on
  // the execution character set (`-fexec-charset`). See hogd issue #2689.
  const char8_t *name;
  uint64_t cost;
};

constexpr std::array<InstructionGasEntry, kOpCount> kInstructionGasSchedule = {{
    {.op = Op::kLui, .name = u8"LUI", .cost = kBaseInstructionGas},
    {.op = Op::kAuipc, .name = u8"AUIPC", .cost = kBaseInstructionGas},
    {.op = Op::kJal, .name = u8"JAL", .cost = kBaseInstructionGas},
    {.op = Op::kJalr, .name = u8"JALR", .cost = kBaseInstructionGas},
    {.op = Op::kBeq, .name = u8"BEQ", .cost = kBaseInstructionGas},
    {.op = Op::kBne, .name = u8"BNE", .cost = kBaseInstructionGas},
    {.op = Op::kBlt, .name = u8"BLT", .cost = kBaseInstructionGas},
    {.op = Op::kBge, .name = u8"BGE", .cost = kBaseInstructionGas},
    {.op = Op::kBltu, .name = u8"BLTU", .cost = kBaseInstructionGas},
    {.op = Op::kBgeu, .name = u8"BGEU", .cost = kBaseInstructionGas},
    {.op = Op::kLb, .name = u8"LB", .cost = kBaseInstructionGas},
    {.op = Op::kLh, .name = u8"LH", .cost = kBaseInstructionGas},
    {.op = Op::kLw, .name = u8"LW", .cost = kBaseInstructionGas},
    {.op = Op::kLbu, .name = u8"LBU", .cost = kBaseInstructionGas},
    {.op = Op::kLhu, .name = u8"LHU", .cost = kBaseInstructionGas},
    {.op = Op::kSb, .name = u8"SB", .cost = kBaseInstructionGas},
    {.op = Op::kSh, .name = u8"SH", .cost = kBaseInstructionGas},
    {.op = Op::kSw, .name = u8"SW", .cost = kBaseInstructionGas},
    {.op = Op::kAddi, .name = u8"ADDI", .cost = kBaseInstructionGas},
    {.op = Op::kSlti, .name = u8"SLTI", .cost = kBaseInstructionGas},
    {.op = Op::kSltiu, .name = u8"SLTIU", .cost = kBaseInstructionGas},
    {.op = Op::kXori, .name = u8"XORI", .cost = kBaseInstructionGas},
    {.op = Op::kOri, .name = u8"ORI", .cost = kBaseInstructionGas},
    {.op = Op::kAndi, .name = u8"ANDI", .cost = kBaseInstructionGas},
    {.op = Op::kSlli, .name = u8"SLLI", .cost = kBaseInstructionGas},
    {.op = Op::kSrli, .name = u8"SRLI", .cost = kBaseInstructionGas},
    {.op = Op::kSrai, .name = u8"SRAI", .cost = kBaseInstructionGas},
    {.op = Op::kAdd, .name = u8"ADD", .cost = kBaseInstructionGas},
    {.op = Op::kSub, .name = u8"SUB", .cost = kBaseInstructionGas},
    {.op = Op::kSll, .name = u8"SLL", .cost = kBaseInstructionGas},
    {.op = Op::kSlt, .name = u8"SLT", .cost = kBaseInstructionGas},
    {.op = Op::kSltu, .name = u8"SLTU", .cost = kBaseInstructionGas},
    {.op = Op::kXor, .name = u8"XOR", .cost = kBaseInstructionGas},
    {.op = Op::kSrl, .name = u8"SRL", .cost = kBaseInstructionGas},
    {.op = Op::kSra, .name = u8"SRA", .cost = kBaseInstructionGas},
    {.op = Op::kOr, .name = u8"OR", .cost = kBaseInstructionGas},
    {.op = Op::kAnd, .name = u8"AND", .cost = kBaseInstructionGas},
    {.op = Op::kMul, .name = u8"MUL", .cost = kMulInstructionGas},
    {.op = Op::kMulh, .name = u8"MULH", .cost = kMulInstructionGas},
    {.op = Op::kMulhsu, .name = u8"MULHSU", .cost = kMulInstructionGas},
    {.op = Op::kMulhu, .name = u8"MULHU", .cost = kMulInstructionGas},
    {.op = Op::kDiv, .name = u8"DIV", .cost = kDivInstructionGas},
    {.op = Op::kDivu, .name = u8"DIVU", .cost = kDivInstructionGas},
    {.op = Op::kRem, .name = u8"REM", .cost = kDivInstructionGas},
    {.op = Op::kRemu, .name = u8"REMU", .cost = kDivInstructionGas},
    {.op = Op::kFence, .name = u8"FENCE", .cost = kBaseInstructionGas},
    {.op = Op::kEcall, .name = u8"ECALL", .cost = kBaseInstructionGas},
    {.op = Op::kEbreak, .name = u8"EBREAK", .cost = kBaseInstructionGas},
    {.op = Op::kUnimp, .name = u8"UNIMP", .cost = kBaseInstructionGas},
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
