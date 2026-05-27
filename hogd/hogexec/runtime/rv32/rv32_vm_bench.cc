#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "hogd/hogexec/runtime/rv32/machine.h"

namespace hogd::rv32 {
namespace {

void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

uint32_t
addi(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return (uint32_t(imm) & 0xfffU) << 20 | uint32_t(rs1) << 15 |
         uint32_t(rd) << 7 | 0x13U;
}

uint32_t
op(uint8_t rd, uint8_t rs1, uint8_t rs2, uint8_t funct3, uint8_t funct7)
{
  return uint32_t(funct7) << 25 | uint32_t(rs2) << 20 | uint32_t(rs1) << 15 |
         uint32_t(funct3) << 12 | uint32_t(rd) << 7 | 0x33U;
}

uint32_t
branch(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  const uint32_t uimm = uint32_t(imm) & 0x1fffU;
  return ((uimm >> 12) & 0x1U) << 31 | ((uimm >> 5) & 0x3fU) << 25 |
         uint32_t(rs2) << 20 | uint32_t(rs1) << 15 | uint32_t(funct3) << 12 |
         ((uimm >> 1) & 0xfU) << 8 | ((uimm >> 11) & 0x1U) << 7 | 0x63U;
}

uint32_t
store(uint8_t rs1, uint8_t rs2, int32_t imm, uint8_t funct3)
{
  const uint32_t uimm = uint32_t(imm) & 0xfffU;
  return ((uimm >> 5) << 25) | uint32_t(rs2) << 20 | uint32_t(rs1) << 15 |
         uint32_t(funct3) << 12 | ((uimm & 0x1fU) << 7) | 0x23U;
}

uint32_t
load(uint8_t rd, uint8_t rs1, int32_t imm, uint8_t funct3)
{
  return (uint32_t(imm) & 0xfffU) << 20 | uint32_t(rs1) << 15 |
         uint32_t(funct3) << 12 | uint32_t(rd) << 7 | 0x03U;
}

std::shared_ptr<const Program>
make_program(std::initializer_list<uint32_t> words)
{
  std::vector<uint8_t> code;
  for (const uint32_t word : words) {
    append_u32(code, word);
  }
  auto program = Program::create(code);
  benchmark::DoNotOptimize(program.has_value());
  return std::make_shared<Program>(std::move(*program));
}

void
bm_dispatch_arithmetic(benchmark::State &state)
{
  const auto program = make_program({
      addi(1, 0, 100),
      addi(2, 0, 1),
      op(3, 3, 2, 0, 0),
      op(4, 3, 2, 0, 1),
      addi(1, 1, -1),
      branch(1, 1, 0, -12),
  });

  for (auto _ : state) {
    Machine machine(program, 4096, 100000);
    while (machine.step() == StepResult::kContinue) {
      benchmark::DoNotOptimize(machine.pc());
    }
    benchmark::DoNotOptimize(machine.reg(3));
  }
  state.SetItemsProcessed(state.iterations() * 303);
}

void
bm_load_store_loop(benchmark::State &state)
{
  const auto program = make_program({
      addi(1, 0, 64),
      addi(2, 0, 100),
      store(1, 2, 0, 2),
      load(3, 1, 0, 2),
      addi(2, 2, -1),
      branch(1, 2, 0, -12),
  });

  for (auto _ : state) {
    Machine machine(program, 4096, 100000);
    while (machine.step() == StepResult::kContinue) {
      benchmark::DoNotOptimize(machine.pc());
    }
    benchmark::DoNotOptimize(machine.reg(3));
  }
  state.SetItemsProcessed(state.iterations() * 303);
}

BENCHMARK(bm_dispatch_arithmetic);
BENCHMARK(bm_load_store_loop);

} // namespace
} // namespace hogd::rv32

BENCHMARK_MAIN();
