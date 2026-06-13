#include "hogd/hogexec/runtime/rv32/program.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/isa_ops.h"

namespace hogd::rv32 {
namespace {

DecodedInstruction
base_decode(uint32_t word)
{
  DecodedInstruction instruction;
  instruction.word = word;
  instruction.rd = rd(word);
  instruction.funct3 = funct3(word);
  instruction.rs1 = rs1(word);
  instruction.rs2 = rs2(word);
  instruction.funct7 = funct7(word);
  return instruction;
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
  const uint32_t op_code = opcode(word);

  switch (op_code) {
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
    if (word == 0x00000073U) {
      instruction.kind = InstructionKind::kEcall;
      instruction.op = Op::kEcall;
      return decoded(instruction);
    }
    if (word == 0x00100073U) {
      instruction.kind = InstructionKind::kEbreak;
      instruction.op = Op::kEbreak;
      return decoded(instruction);
    }
    if (word == 0xc0001073U) {
      instruction.kind = InstructionKind::kEbreak;
      instruction.op = Op::kUnimp;
      return decoded(instruction);
    }
    // ECALL, EBREAK, and canonical UNIMP are the accepted SYSTEM instructions.
    // CSR, privileged, and environment-return forms are rejected before a
    // Program can be created.
    return unsupported(ProgramError::kUnsupportedSystemInstruction);
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
    const DecodeResult result = decode_instruction(load_u32_le(code, offset));
    assert(result.ok);
    program.instructions_.push_back(result.instruction);
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
