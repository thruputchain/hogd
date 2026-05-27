#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/encoding.h"

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

struct InstructionGasEntry {
  Op op;
  const char *name;
  uint64_t cost;
};

constexpr std::array<InstructionGasEntry, kOpCount> kInstructionGasSchedule = {{
    {Op::kLui, "LUI", kBaseInstructionGas},
    {Op::kAuipc, "AUIPC", kBaseInstructionGas},
    {Op::kJal, "JAL", kBaseInstructionGas},
    {Op::kJalr, "JALR", kBaseInstructionGas},
    {Op::kBeq, "BEQ", kBaseInstructionGas},
    {Op::kBne, "BNE", kBaseInstructionGas},
    {Op::kBlt, "BLT", kBaseInstructionGas},
    {Op::kBge, "BGE", kBaseInstructionGas},
    {Op::kBltu, "BLTU", kBaseInstructionGas},
    {Op::kBgeu, "BGEU", kBaseInstructionGas},
    {Op::kLb, "LB", kBaseInstructionGas},
    {Op::kLh, "LH", kBaseInstructionGas},
    {Op::kLw, "LW", kBaseInstructionGas},
    {Op::kLbu, "LBU", kBaseInstructionGas},
    {Op::kLhu, "LHU", kBaseInstructionGas},
    {Op::kSb, "SB", kBaseInstructionGas},
    {Op::kSh, "SH", kBaseInstructionGas},
    {Op::kSw, "SW", kBaseInstructionGas},
    {Op::kAddi, "ADDI", kBaseInstructionGas},
    {Op::kSlti, "SLTI", kBaseInstructionGas},
    {Op::kSltiu, "SLTIU", kBaseInstructionGas},
    {Op::kXori, "XORI", kBaseInstructionGas},
    {Op::kOri, "ORI", kBaseInstructionGas},
    {Op::kAndi, "ANDI", kBaseInstructionGas},
    {Op::kSlli, "SLLI", kBaseInstructionGas},
    {Op::kSrli, "SRLI", kBaseInstructionGas},
    {Op::kSrai, "SRAI", kBaseInstructionGas},
    {Op::kAdd, "ADD", kBaseInstructionGas},
    {Op::kSub, "SUB", kBaseInstructionGas},
    {Op::kSll, "SLL", kBaseInstructionGas},
    {Op::kSlt, "SLT", kBaseInstructionGas},
    {Op::kSltu, "SLTU", kBaseInstructionGas},
    {Op::kXor, "XOR", kBaseInstructionGas},
    {Op::kSrl, "SRL", kBaseInstructionGas},
    {Op::kSra, "SRA", kBaseInstructionGas},
    {Op::kOr, "OR", kBaseInstructionGas},
    {Op::kAnd, "AND", kBaseInstructionGas},
    {Op::kMul, "MUL", kMulInstructionGas},
    {Op::kMulh, "MULH", kMulInstructionGas},
    {Op::kMulhsu, "MULHSU", kMulInstructionGas},
    {Op::kMulhu, "MULHU", kMulInstructionGas},
    {Op::kDiv, "DIV", kDivInstructionGas},
    {Op::kDivu, "DIVU", kDivInstructionGas},
    {Op::kRem, "REM", kDivInstructionGas},
    {Op::kRemu, "REMU", kDivInstructionGas},
    {Op::kFence, "FENCE", kBaseInstructionGas},
    {Op::kEcall, "ECALL", kBaseInstructionGas},
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
gas_schedule_digest_byte(uint64_t digest, uint8_t value)
{
  return fnv_digest_byte(digest, value);
}

constexpr uint64_t
gas_schedule_digest_u64(uint64_t digest, uint64_t value)
{
  return fnv_digest_u64(digest, value);
}

constexpr uint64_t
gas_schedule_digest()
{
  uint64_t digest = kFnvOffsetBasis;
  digest = gas_schedule_digest_u64(digest, kGasTableVersion);
  digest = gas_schedule_digest_u64(digest, kOpCount);
  for (size_t i = 0; i < kInstructionGasSchedule.size(); ++i) {
    const auto &entry = kInstructionGasSchedule[i];
    digest = gas_schedule_digest_u64(digest, i);
    digest = gas_schedule_digest_u64(digest, static_cast<size_t>(entry.op));
    digest = gas_schedule_digest_u64(digest, entry.cost);
    for (const char *cursor = entry.name; *cursor != '\0'; ++cursor) {
      digest = gas_schedule_digest_byte(digest, static_cast<uint8_t>(*cursor));
    }
    digest = gas_schedule_digest_byte(digest, 0);
  }
  return digest;
}

constexpr uint64_t kGasScheduleDigest = gas_schedule_digest();

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
