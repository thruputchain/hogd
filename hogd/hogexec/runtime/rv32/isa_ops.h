#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>

#include "hogd/hogexec/runtime/rv32/program.h"

// Pure, constexpr ISA helpers shared across the RV32 machine lineage (the
// canonical Machine and future experimental machines). Decode-field extraction
// and execution arithmetic only -- no machine/VM state. The independent spec
// model (spec_model.h) deliberately does NOT use this header; it keeps its own
// copies so Machine-vs-spec differential tests stay meaningful.
namespace hogd::rv32 {

// --- Reinterpret -----------------------------------------------------------
constexpr int32_t
i32_from_bits(uint32_t value)
{
  return std::bit_cast<int32_t>(value);
}

constexpr uint32_t
bits_from_i32(int32_t value)
{
  return std::bit_cast<uint32_t>(value);
}

// --- Instruction-field decode primitives -----------------------------------
constexpr uint32_t
bits(uint32_t value, uint32_t low, uint32_t width)
{
  return (value >> low) & ((uint32_t{1} << width) - 1);
}

constexpr uint32_t
sign_extend(uint32_t value, uint32_t bits_count)
{
  const uint32_t sign_bit = uint32_t{1} << (bits_count - 1);
  return (value ^ sign_bit) - sign_bit;
}

template<uint32_t Bits>
constexpr uint32_t
sign_extend(uint32_t value)
{
  constexpr static uint32_t kSignBit = uint32_t{1} << ((Bits)-1);
  return (value ^ kSignBit) - kSignBit;
}

constexpr uint32_t
opcode(uint32_t word)
{
  return word & 0x7fU;
}

constexpr uint8_t
rd(uint32_t word)
{
  return static_cast<uint8_t>(bits(word, 7, 5));
}

constexpr uint8_t
funct3(uint32_t word)
{
  return static_cast<uint8_t>(bits(word, 12, 3));
}

constexpr uint8_t
rs1(uint32_t word)
{
  return static_cast<uint8_t>(bits(word, 15, 5));
}

constexpr uint8_t
rs2(uint32_t word)
{
  return static_cast<uint8_t>(bits(word, 20, 5));
}

constexpr uint8_t
funct7(uint32_t word)
{
  return static_cast<uint8_t>(bits(word, 25, 7));
}

constexpr int32_t
imm_i(uint32_t word)
{
  return i32_from_bits(sign_extend(bits(word, 20, 12), 12));
}

constexpr int32_t
imm_s(uint32_t word)
{
  return i32_from_bits(
      sign_extend((bits(word, 25, 7) << 5) | bits(word, 7, 5), 12));
}

constexpr int32_t
imm_b(uint32_t word)
{
  const uint32_t value = (bits(word, 31, 1) << 12) | (bits(word, 7, 1) << 11) |
                         (bits(word, 25, 6) << 5) | (bits(word, 8, 4) << 1);
  return i32_from_bits(sign_extend(value, 13));
}

constexpr int32_t
imm_u(uint32_t word)
{
  return i32_from_bits(word & 0xfffff000U);
}

constexpr int32_t
imm_j(uint32_t word)
{
  const uint32_t value = (bits(word, 31, 1) << 20) | (bits(word, 12, 8) << 12) |
                         (bits(word, 20, 1) << 11) | (bits(word, 21, 10) << 1);
  return i32_from_bits(sign_extend(value, 21));
}

// --- Execution arithmetic ---------------------------------------------------
constexpr uint32_t
arithmetic_shift_right(uint32_t value, uint32_t amount)
{
  // C++20 mandates two's complement and defines signed right shift as an
  // arithmetic shift. Callers mask the shift amount to 5 bits, so amount is in
  // [0, 31] and there is no UB. This lowers to a single arithmetic-shift
  // instruction; the prior unsigned-plus-sign-fill form blinded the compiler.
  assert(amount <= 31 && "shift amount must be masked to 5 bits by the caller");
  return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
}

constexpr uint32_t
mulh(uint32_t lhs, uint32_t rhs)
{
  // MULH variants differ only in signedness of the high half. Keep the casts
  // explicit so future refactors do not accidentally collapse the variants.
  const int64_t product =
      int64_t{i32_from_bits(lhs)} * int64_t{i32_from_bits(rhs)};
  return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
}

constexpr uint32_t
mulhsu(uint32_t lhs, uint32_t rhs)
{
  const int64_t product = int64_t{i32_from_bits(lhs)} * int64_t{rhs};
  return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
}

constexpr uint32_t
mulhu(uint32_t lhs, uint32_t rhs)
{
  const uint64_t product = uint64_t{lhs} * uint64_t{rhs};
  return static_cast<uint32_t>(product >> 32);
}

constexpr uint32_t
div_signed(uint32_t lhs, uint32_t rhs)
{
  const int32_t divisor = i32_from_bits(rhs);
  const int32_t dividend = i32_from_bits(lhs);
  // RISC-V defines division-by-zero and signed-overflow results; avoid C++ UB.
  if (divisor == 0) {
    return std::numeric_limits<uint32_t>::max();
  }
  if (dividend == std::numeric_limits<int32_t>::min() && divisor == -1) {
    return lhs;
  }
  return bits_from_i32(static_cast<int32_t>(dividend / divisor));
}

constexpr uint32_t
rem_signed(uint32_t lhs, uint32_t rhs)
{
  const int32_t divisor = i32_from_bits(rhs);
  const int32_t dividend = i32_from_bits(lhs);
  // RISC-V defines division-by-zero and signed-overflow results; avoid C++ UB.
  if (divisor == 0) {
    return lhs;
  }
  if (dividend == std::numeric_limits<int32_t>::min() && divisor == -1) {
    return 0;
  }
  return bits_from_i32(static_cast<int32_t>(dividend % divisor));
}

constexpr bool
branch_taken(Op op, uint32_t lhs, uint32_t rhs)
{
  switch (op) {
  case Op::kBeq:
    return lhs == rhs;
  case Op::kBne:
    return lhs != rhs;
  case Op::kBlt:
    return i32_from_bits(lhs) < i32_from_bits(rhs);
  case Op::kBge:
    return i32_from_bits(lhs) >= i32_from_bits(rhs);
  case Op::kBltu:
    return lhs < rhs;
  case Op::kBgeu:
    return lhs >= rhs;
  default:
    return false;
  }
}

} // namespace hogd::rv32
