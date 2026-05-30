#pragma once

#include <bit>
#include <cstdint>

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
  return i32_from_bits(sign_extend((bits(word, 25, 7) << 5) | bits(word, 7, 5), 12));
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

} // namespace hogd::rv32
