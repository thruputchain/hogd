#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/isa_ops.h"
#include "hogd/hogexec/runtime/rv32/program.h"

// Independent, compact decode for the direct-threaded interpreter. Separate
// from Program: its own opcode->Op classification (via isa_ops.h field
// extractors) so the threaded-vs-canonical differential test covers decode.
namespace hogd::rv32 {

// Compact hot-path instruction. No word/funct3/funct7 carried into execution.
struct Instr {
  Op op = Op::kAdd;
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  int32_t imm = 0;
  uint64_t gas = kBaseInstructionGas;
};

class ThreadedProgram {
public:
  static std::expected<ThreadedProgram, ProgramError>
  create(std::span<const uint8_t> code)
  {
    if ((code.size() % kInstructionBytes) != 0) {
      return std::unexpected(ProgramError::kCodeSizeNotAligned);
    }
    if (code.size() > kMaxCodeBytes) {
      return std::unexpected(ProgramError::kCodeTooLarge);
    }
    ThreadedProgram program;
    program.code_.reserve(code.size() / kInstructionBytes);
    for (size_t off = 0; off < code.size(); off += kInstructionBytes) {
      const auto decoded = decode_one(load_u32_le(code, off));
      if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
      }
      program.code_.push_back(decoded.value());
    }
    return program;
  }

  [[nodiscard]] size_t instruction_count() const { return code_.size(); }
  [[nodiscard]] std::span<const Instr> code() const { return code_; }
  [[nodiscard]] bool contains_pc(uint32_t pc) const
  {
    return (pc % kInstructionBytes) == 0 &&
           pc / kInstructionBytes < code_.size();
  }

private:
  static std::expected<Instr, ProgramError> decode_one(uint32_t word)
  {
    if ((word & 0x3U) != 0x3U) {
      return std::unexpected(ProgramError::kUnsupportedCompressedInstruction);
    }
    Instr in;
    in.rd = rd(word);
    in.rs1 = rs1(word);
    in.rs2 = rs2(word);
    const uint8_t f3 = funct3(word);
    const uint8_t f7 = funct7(word);
    auto ok = [&](Op op, int32_t imm) -> std::expected<Instr, ProgramError> {
      in.op = op;
      in.imm = imm;
      in.gas = gas_cost_for_op(op);
      return in;
    };
    auto bad = [](ProgramError e) -> std::expected<Instr, ProgramError> {
      return std::unexpected(e);
    };
    switch (opcode(word)) {
    case 0x37:
      return ok(Op::kLui, imm_u(word));
    case 0x17:
      return ok(Op::kAuipc, imm_u(word));
    case 0x6f:
      return ok(Op::kJal, imm_j(word));
    case 0x67:
      if (f3 != 0) {
        return bad(ProgramError::kUnsupportedInstruction);
      }
      return ok(Op::kJalr, imm_i(word));
    case 0x63:
      switch (f3) {
      case 0:
        return ok(Op::kBeq, imm_b(word));
      case 1:
        return ok(Op::kBne, imm_b(word));
      case 4:
        return ok(Op::kBlt, imm_b(word));
      case 5:
        return ok(Op::kBge, imm_b(word));
      case 6:
        return ok(Op::kBltu, imm_b(word));
      case 7:
        return ok(Op::kBgeu, imm_b(word));
      default:
        return bad(ProgramError::kUnsupportedInstruction);
      }
    case 0x03:
      switch (f3) {
      case 0:
        return ok(Op::kLb, imm_i(word));
      case 1:
        return ok(Op::kLh, imm_i(word));
      case 2:
        return ok(Op::kLw, imm_i(word));
      case 4:
        return ok(Op::kLbu, imm_i(word));
      case 5:
        return ok(Op::kLhu, imm_i(word));
      default:
        return bad(ProgramError::kUnsupportedInstruction);
      }
    case 0x23:
      switch (f3) {
      case 0:
        return ok(Op::kSb, imm_s(word));
      case 1:
        return ok(Op::kSh, imm_s(word));
      case 2:
        return ok(Op::kSw, imm_s(word));
      default:
        return bad(ProgramError::kUnsupportedInstruction);
      }
    case 0x13:
      switch (f3) {
      case 0:
        return ok(Op::kAddi, imm_i(word));
      case 2:
        return ok(Op::kSlti, imm_i(word));
      case 3:
        return ok(Op::kSltiu, imm_i(word));
      case 4:
        return ok(Op::kXori, imm_i(word));
      case 6:
        return ok(Op::kOri, imm_i(word));
      case 7:
        return ok(Op::kAndi, imm_i(word));
      case 1:
        if (f7 != 0) {
          return bad(ProgramError::kUnsupportedInstruction);
        }
        return ok(Op::kSlli, static_cast<int32_t>(in.rs2));
      case 5:
        if (f7 == 0) {
          return ok(Op::kSrli, static_cast<int32_t>(in.rs2));
        }
        if (f7 == 0x20) {
          return ok(Op::kSrai, static_cast<int32_t>(in.rs2));
        }
        return bad(ProgramError::kUnsupportedInstruction);
      default:
        return bad(ProgramError::kUnsupportedInstruction);
      }
    case 0x33:
      if (f7 == 0) {
        static constexpr Op kOps[8] = {Op::kAdd, Op::kSll, Op::kSlt, Op::kSltu,
                                       Op::kXor, Op::kSrl, Op::kOr,  Op::kAnd};
        return ok(kOps[f3], 0);
      }
      if (f7 == 0x20 && f3 == 0) {
        return ok(Op::kSub, 0);
      }
      if (f7 == 0x20 && f3 == 5) {
        return ok(Op::kSra, 0);
      }
      if (f7 == 0x01) {
        static constexpr Op kOps[8] = {Op::kMul,   Op::kMulh, Op::kMulhsu,
                                       Op::kMulhu, Op::kDiv,  Op::kDivu,
                                       Op::kRem,   Op::kRemu};
        return ok(kOps[f3], 0);
      }
      return bad(ProgramError::kUnsupportedInstruction);
    case 0x0f:
      if (f3 != 0) {
        return bad(ProgramError::kUnsupportedInstruction);
      }
      return ok(Op::kFence, 0);
    case 0x73:
      if (word == 0x00000073U) {
        return ok(Op::kEcall, 0);
      }
      if (word == 0x00100073U) {
        return ok(Op::kEbreak, 0);
      }
      return bad(ProgramError::kUnsupportedSystemInstruction);
    default:
      return bad(ProgramError::kUnsupportedInstruction);
    }
  }

  std::vector<Instr> code_;
};

} // namespace hogd::rv32
