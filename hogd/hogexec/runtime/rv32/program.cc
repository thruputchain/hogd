#include "hogd/hogexec/runtime/rv32/program.h"

#include <array>
#include <bit>

namespace hogd::rv32 {
namespace {

constexpr uint32_t kOpcodeMask = 0x7f;

uint32_t
bits(uint32_t value, uint32_t low, uint32_t width)
{
  return (value >> low) & ((uint32_t{1} << width) - 1);
}

int32_t
sign_extend(uint32_t value, uint32_t bits_count)
{
  const uint32_t sign_bit = uint32_t{1} << (bits_count - 1);
  const int64_t extended = (value & sign_bit) != 0
                               ? int64_t{value} - (int64_t{1} << bits_count)
                               : int64_t{value};
  return static_cast<int32_t>(extended);
}

uint32_t
load_u32_le(std::span<const uint8_t> code, size_t offset)
{
  return uint32_t{code[offset]} | (uint32_t{code[offset + 1]} << 8) |
         (uint32_t{code[offset + 2]} << 16) |
         (uint32_t{code[offset + 3]} << 24);
}

DecodedInstruction
base_decode(uint32_t word)
{
  DecodedInstruction instruction;
  instruction.word = word;
  instruction.rd = static_cast<uint8_t>(bits(word, 7, 5));
  instruction.funct3 = static_cast<uint8_t>(bits(word, 12, 3));
  instruction.rs1 = static_cast<uint8_t>(bits(word, 15, 5));
  instruction.rs2 = static_cast<uint8_t>(bits(word, 20, 5));
  instruction.funct7 = static_cast<uint8_t>(bits(word, 25, 7));
  return instruction;
}

int32_t
imm_i(uint32_t word)
{
  return sign_extend(bits(word, 20, 12), 12);
}

int32_t
imm_s(uint32_t word)
{
  return sign_extend((bits(word, 25, 7) << 5) | bits(word, 7, 5), 12);
}

int32_t
imm_b(uint32_t word)
{
  const uint32_t value = (bits(word, 31, 1) << 12) | (bits(word, 7, 1) << 11) |
                         (bits(word, 25, 6) << 5) | (bits(word, 8, 4) << 1);
  return sign_extend(value, 13);
}

int32_t
imm_u(uint32_t word)
{
  return std::bit_cast<int32_t>(word & 0xfffff000U);
}

int32_t
imm_j(uint32_t word)
{
  const uint32_t value = (bits(word, 31, 1) << 20) | (bits(word, 12, 8) << 12) |
                         (bits(word, 20, 1) << 11) | (bits(word, 21, 10) << 1);
  return sign_extend(value, 21);
}

DecodeResult
unsupported(ProgramError error)
{
  return DecodeResult{.ok = false, .error = error};
}

DecodeResult
decoded(DecodedInstruction instruction)
{
  // Gas is a property of the validated decoded stream. Execution never
  // re-decodes or recomputes costs from raw bytes.
  instruction.gas_cost = gas_cost_for_op(instruction.op);
  return DecodeResult{.ok = true,
                      .error = ProgramError::kNone,
                      .instruction = instruction};
}

} // namespace

DecodeResult
decode_instruction(uint32_t word)
{
  if ((word & 0x3U) != 0x3U) {
    // The v1 profile excludes compressed instructions so every PC is a
    // four-byte index into the immutable decoded instruction array.
    return unsupported(ProgramError::kUnsupportedCompressedInstruction);
  }

  auto instruction = base_decode(word);
  const uint32_t opcode = word & kOpcodeMask;

  switch (opcode) {
  case 0x37:
    instruction.kind = InstructionKind::kLui;
    instruction.op = Op::kLui;
    instruction.immediate = imm_u(word);
    return decoded(instruction);
  case 0x17:
    instruction.kind = InstructionKind::kAuipc;
    instruction.op = Op::kAuipc;
    instruction.immediate = imm_u(word);
    return decoded(instruction);
  case 0x6f:
    instruction.kind = InstructionKind::kJal;
    instruction.op = Op::kJal;
    instruction.immediate = imm_j(word);
    return decoded(instruction);
  case 0x67:
    if (instruction.funct3 != 0) {
      return unsupported(ProgramError::kUnsupportedInstruction);
    }
    instruction.kind = InstructionKind::kJalr;
    instruction.op = Op::kJalr;
    instruction.immediate = imm_i(word);
    return decoded(instruction);
  case 0x63:
    instruction.kind = InstructionKind::kBranch;
    instruction.immediate = imm_b(word);
    switch (instruction.funct3) {
    case 0:
      instruction.op = Op::kBeq;
      return decoded(instruction);
    case 1:
      instruction.op = Op::kBne;
      return decoded(instruction);
    case 4:
      instruction.op = Op::kBlt;
      return decoded(instruction);
    case 5:
      instruction.op = Op::kBge;
      return decoded(instruction);
    case 6:
      instruction.op = Op::kBltu;
      return decoded(instruction);
    case 7:
      instruction.op = Op::kBgeu;
      return decoded(instruction);
    default:
      return unsupported(ProgramError::kUnsupportedInstruction);
    }
  case 0x03:
    instruction.kind = InstructionKind::kLoad;
    instruction.immediate = imm_i(word);
    switch (instruction.funct3) {
    case 0:
      instruction.op = Op::kLb;
      return decoded(instruction);
    case 1:
      instruction.op = Op::kLh;
      return decoded(instruction);
    case 2:
      instruction.op = Op::kLw;
      return decoded(instruction);
    case 4:
      instruction.op = Op::kLbu;
      return decoded(instruction);
    case 5:
      instruction.op = Op::kLhu;
      return decoded(instruction);
    default:
      return unsupported(ProgramError::kUnsupportedInstruction);
    }
  case 0x23:
    instruction.kind = InstructionKind::kStore;
    instruction.immediate = imm_s(word);
    switch (instruction.funct3) {
    case 0:
      instruction.op = Op::kSb;
      return decoded(instruction);
    case 1:
      instruction.op = Op::kSh;
      return decoded(instruction);
    case 2:
      instruction.op = Op::kSw;
      return decoded(instruction);
    default:
      return unsupported(ProgramError::kUnsupportedInstruction);
    }
  case 0x13:
    instruction.kind = InstructionKind::kOpImm;
    instruction.immediate = imm_i(word);
    switch (instruction.funct3) {
    case 0:
      instruction.op = Op::kAddi;
      return decoded(instruction);
    case 2:
      instruction.op = Op::kSlti;
      return decoded(instruction);
    case 3:
      instruction.op = Op::kSltiu;
      return decoded(instruction);
    case 4:
      instruction.op = Op::kXori;
      return decoded(instruction);
    case 6:
      instruction.op = Op::kOri;
      return decoded(instruction);
    case 7:
      instruction.op = Op::kAndi;
      return decoded(instruction);
    case 1:
      if (instruction.funct7 != 0) {
        return unsupported(ProgramError::kUnsupportedInstruction);
      }
      instruction.op = Op::kSlli;
      instruction.immediate = static_cast<int32_t>(instruction.rs2);
      return decoded(instruction);
    case 5:
      if (instruction.funct7 == 0) {
        instruction.op = Op::kSrli;
        instruction.immediate = static_cast<int32_t>(instruction.rs2);
        return decoded(instruction);
      }
      if (instruction.funct7 == 0x20) {
        instruction.op = Op::kSrai;
        instruction.immediate = static_cast<int32_t>(instruction.rs2);
        return decoded(instruction);
      }
      return unsupported(ProgramError::kUnsupportedInstruction);
    default:
      return unsupported(ProgramError::kUnsupportedInstruction);
    }
  case 0x33:
    instruction.kind = InstructionKind::kOp;
    if (instruction.funct7 == 0) {
      static constexpr std::array<Op, 8> kOps = {Op::kAdd,  Op::kSll, Op::kSlt,
                                                 Op::kSltu, Op::kXor, Op::kSrl,
                                                 Op::kOr,   Op::kAnd};
      instruction.op = kOps[instruction.funct3];
      return decoded(instruction);
    }
    if (instruction.funct7 == 0x20 && instruction.funct3 == 0) {
      instruction.op = Op::kSub;
      return decoded(instruction);
    }
    if (instruction.funct7 == 0x20 && instruction.funct3 == 5) {
      instruction.op = Op::kSra;
      return decoded(instruction);
    }
    if (instruction.funct7 == 0x01) {
      static constexpr std::array<Op, 8> kOps = {Op::kMul,    Op::kMulh,
                                                 Op::kMulhsu, Op::kMulhu,
                                                 Op::kDiv,    Op::kDivu,
                                                 Op::kRem,    Op::kRemu};
      instruction.op = kOps[instruction.funct3];
      return decoded(instruction);
    }
    return unsupported(ProgramError::kUnsupportedInstruction);
  case 0x0f:
    if (instruction.funct3 != 0) {
      return unsupported(ProgramError::kUnsupportedInstruction);
    }
    instruction.kind = InstructionKind::kFence;
    instruction.op = Op::kFence;
    return decoded(instruction);
  case 0x73:
    if (word != 0x00000073U) {
      // ECALL is the only accepted SYSTEM instruction. CSR, privileged, and
      // environment-return forms are rejected before a Program can be created.
      return unsupported(ProgramError::kUnsupportedSystemInstruction);
    }
    instruction.kind = InstructionKind::kEcall;
    instruction.op = Op::kEcall;
    return decoded(instruction);
  default:
    return unsupported(ProgramError::kUnsupportedInstruction);
  }
}

ProgramResult
Program::validate(std::span<const uint8_t> code)
{
  // Validation is the consensus boundary for the RV32 profile. A Machine only
  // receives already-decoded instructions from Program::create.
  if ((code.size() % kInstructionBytes) != 0) {
    return ProgramResult{.ok = false,
                         .error = ProgramError::kCodeSizeNotAligned};
  }
  if (code.size() > kMaxCodeBytes) {
    return ProgramResult{.ok = false, .error = ProgramError::kCodeTooLarge};
  }

  for (size_t offset = 0; offset < code.size(); offset += kInstructionBytes) {
    const auto decoded_instruction =
        decode_instruction(load_u32_le(code, offset));
    if (!decoded_instruction.ok) {
      return ProgramResult{.ok = false, .error = decoded_instruction.error};
    }
  }
  return ProgramResult{.ok = true, .error = ProgramError::kNone};
}

std::expected<Program, ProgramError>
Program::create(std::span<const uint8_t> code)
{
  const auto result = validate(code);
  if (!result.ok) {
    return std::unexpected(result.error);
  }

  Program program;
  program.instructions_.reserve(code.size() / kInstructionBytes);
  // Store only decoded instructions. The raw guest code bytes are intentionally
  // not guest-addressable and cannot be modified after validation.
  for (size_t offset = 0; offset < code.size(); offset += kInstructionBytes) {
    program.instructions_.push_back(
        decode_instruction(load_u32_le(code, offset)).instruction);
  }
  return program;
}

bool
Program::contains_pc(uint32_t pc) const
{
  if ((pc % kInstructionBytes) != 0) {
    return false;
  }
  return pc / kInstructionBytes < instructions_.size();
}

const DecodedInstruction &
Program::instruction_at_aligned_pc(uint32_t pc) const
{
  return instructions_[pc / kInstructionBytes];
}

} // namespace hogd::rv32
