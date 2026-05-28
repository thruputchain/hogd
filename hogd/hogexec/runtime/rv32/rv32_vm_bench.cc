#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/rust/wasmi/bindings.h"

namespace hogd::rv32 {
namespace {

constexpr uint32_t kBenchMemoryBytes = 4096;
constexpr uint64_t kBenchGasLimit = 1'000'000'000;
constexpr uint32_t kFibInput = 20;
constexpr uint32_t kFib20Result = 6765;

void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

uint32_t
i_type(uint32_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  return (static_cast<uint32_t>(imm) & 0xfffU) << 20 |
         static_cast<uint32_t>(rs1) << 15 |
         static_cast<uint32_t>(funct3) << 12 | static_cast<uint32_t>(rd) << 7 |
         opcode;
}

uint32_t
j_type(uint8_t rd, int32_t imm)
{
  const uint32_t uimm = static_cast<uint32_t>(imm) & 0x1fffffU;
  return ((uimm >> 20) & 0x1U) << 31 | ((uimm >> 1) & 0x3ffU) << 21 |
         ((uimm >> 11) & 0x1U) << 20 | ((uimm >> 12) & 0xffU) << 12 |
         static_cast<uint32_t>(rd) << 7 | 0x6fU;
}

uint32_t
addi(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x13U, rd, 0, rs1, imm);
}

uint32_t
op(uint8_t rd, uint8_t rs1, uint8_t rs2, uint8_t funct3, uint8_t funct7)
{
  return static_cast<uint32_t>(funct7) << 25 |
         static_cast<uint32_t>(rs2) << 20 | static_cast<uint32_t>(rs1) << 15 |
         static_cast<uint32_t>(funct3) << 12 | static_cast<uint32_t>(rd) << 7 |
         0x33U;
}

uint32_t
branch(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  const uint32_t uimm = static_cast<uint32_t>(imm) & 0x1fffU;
  return ((uimm >> 12) & 0x1U) << 31 | ((uimm >> 5) & 0x3fU) << 25 |
         static_cast<uint32_t>(rs2) << 20 | static_cast<uint32_t>(rs1) << 15 |
         static_cast<uint32_t>(funct3) << 12 | ((uimm >> 1) & 0xfU) << 8 |
         ((uimm >> 11) & 0x1U) << 7 | 0x63U;
}

uint32_t
store(uint8_t rs1, uint8_t rs2, int32_t imm, uint8_t funct3)
{
  const uint32_t uimm = static_cast<uint32_t>(imm) & 0xfffU;
  return ((uimm >> 5) << 25) | static_cast<uint32_t>(rs2) << 20 |
         static_cast<uint32_t>(rs1) << 15 |
         static_cast<uint32_t>(funct3) << 12 | ((uimm & 0x1fU) << 7) | 0x23U;
}

uint32_t
load(uint8_t rd, uint8_t rs1, int32_t imm, uint8_t funct3)
{
  return i_type(0x03U, rd, funct3, rs1, imm);
}

uint32_t
jalr(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x67U, rd, 0, rs1, imm);
}

std::shared_ptr<const Program>
make_program(std::span<const uint32_t> words)
{
  std::vector<uint8_t> code;
  for (const uint32_t word : words) {
    append_u32(code, word);
  }
  auto program = Program::create(code);
  if (!program.has_value()) {
    std::abort();
  }
  return std::make_shared<Program>(std::move(*program));
}

std::shared_ptr<const Program>
make_program(std::initializer_list<uint32_t> words)
{
  return make_program(std::span<const uint32_t>(words.begin(), words.size()));
}

class HaltHost : public Host {
public:
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)args;
    return HostResult::halt(view.reg(10));
  }
};

struct Rv32RunStats {
  StepResult result = StepResult::kTrap;
  uint64_t instructions = 0;
  uint64_t gas_consumed = 0;
};

Rv32RunStats
run_rv32_program(Machine &machine, Host &host, uint64_t gas_limit)
{
  uint64_t instructions = 0;
  for (;;) {
    const StepResult result = machine.step();
    switch (result) {
    case StepResult::kContinue:
      ++instructions;
      continue;
    case StepResult::kEcall:
    {
      ++instructions;
      const auto pending_ecall = machine.pending_ecall();
      if (!pending_ecall.has_value()) {
        std::abort();
      }
      const EcallArgs ecall_args = pending_ecall.value();
      const HostResult host_result = host.ecall(machine.view(), ecall_args);
      const StepResult completed = machine.complete_ecall(host_result);
      if (completed == StepResult::kContinue) {
        continue;
      }
      return Rv32RunStats{.result = completed,
                          .instructions = instructions,
                          .gas_consumed = gas_limit - machine.gas_remaining()};
    }
    case StepResult::kHalted:
    case StepResult::kTrap:
      return Rv32RunStats{.result = result,
                          .instructions = instructions,
                          .gas_consumed = gas_limit - machine.gas_remaining()};
    default:
      std::abort();
    }
  }
}

void
run_rv32_benchmark(benchmark::State &state,
                   const std::shared_ptr<const Program> &program)
{
  Machine machine(program, kBenchMemoryBytes, kBenchGasLimit);
  HaltHost host;
  int64_t executed_instructions = 0;
  int64_t consumed_gas = 0;

  for (auto _ : state) {
    state.PauseTiming();
    machine.reset(kBenchGasLimit);
    state.ResumeTiming();

    const Rv32RunStats stats = run_rv32_program(machine, host, kBenchGasLimit);
    if (stats.result != StepResult::kHalted) {
      std::abort();
    }
    executed_instructions += static_cast<int64_t>(stats.instructions);
    consumed_gas += static_cast<int64_t>(stats.gas_consumed);
    benchmark::DoNotOptimize(machine.halt_code());
  }

  state.SetItemsProcessed(executed_instructions);
  state.counters["gas"] =
      benchmark::Counter(static_cast<double>(consumed_gas),
                         benchmark::Counter::kAvgIterations);
}

std::shared_ptr<const Program>
make_recursive_fibonacci_program()
{
  constexpr uint8_t kRa = 1;
  constexpr uint8_t kSp = 2;
  constexpr uint8_t kT0 = 5;
  constexpr uint8_t kT1 = 6;
  constexpr uint8_t kA0 = 10;

  return make_program({
      addi(kA0, 0, kFibInput), j_type(kRa, 8),          0x00000073U,

      addi(kSp, kSp, -16),     store(kSp, kRa, 12, 2),  store(kSp, kA0, 8, 2),
      addi(kT0, 0, 2),         branch(4, kA0, kT0, 36), addi(kA0, kA0, -1),
      j_type(kRa, -24),        store(kSp, kA0, 4, 2),   load(kA0, kSp, 8, 2),
      addi(kA0, kA0, -2),      j_type(kRa, -40),        load(kT1, kSp, 4, 2),
      op(kA0, kA0, kT1, 0, 0), load(kRa, kSp, 12, 2),   addi(kSp, kSp, 16),
      jalr(0, kRa, 0),
  });
}

struct WasmiBenchContract {
  std::array<uint8_t, 32> hash = {0x72, 0x76, 0x33, 0x32, 0x2d, 0x66, 0x69,
                                  0x62, 0x6f, 0x6e, 0x61, 0x63, 0x63, 0x69,
                                  0x2d, 0x72, 0x61, 0x77, 0x2d, 0x62, 0x65,
                                  0x6e, 0x63, 0x68, 0x00, 0x00, 0x00, 0x00,
                                  0x14, 0x00, 0x00, 0x01};
  std::vector<uint8_t> wasm;
};

std::vector<uint8_t>
load_wasm_from_dir(std::string_view directory, std::string_view filename)
{
  const std::string path = std::string(directory) + "/" + std::string(filename);
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    std::abort();
  }

  std::vector<uint8_t> bytes;
  std::array<uint8_t, 65536> buffer = {};
  size_t count = 0;
  while ((count = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
    bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
  }
  std::fclose(file);
  return bytes;
}

extern "C" ContractGetResult
wasmi_bench_contract_get(void *userctx, const uint8_t *hash_ptr) noexcept
{
  if (userctx == nullptr || hash_ptr == nullptr) {
    return ContractGetResult{.err_code = 1, .data = nullptr, .data_len = 0};
  }
  auto *contract = static_cast<WasmiBenchContract *>(userctx);
  if (std::memcmp(hash_ptr, contract->hash.data(), contract->hash.size()) !=
      0) {
    return ContractGetResult{.err_code = 0, .data = nullptr, .data_len = 0};
  }
  return ContractGetResult{.err_code = 0,
                           .data = contract->wasm.data(),
                           .data_len = contract->wasm.size()};
}

class WasmiBenchRuntime {
public:
  explicit WasmiBenchRuntime(WasmiBenchContract &contract) : contract_(contract)
  {
    global_context_ = new_wasmi_global_context();
    context_ = new_wasmi_context(&wasmi_bench_contract_get, global_context_);
    const NewWasmiRuntimeResult result =
        new_wasmi_runtime(contract_.hash.data(), &contract_, context_);
    if (result.err_code != 0 || result.context_pointer == nullptr) {
      std::abort();
    }
    runtime_ = result.context_pointer;
  }

  WasmiBenchRuntime(const WasmiBenchRuntime &) = delete;
  WasmiBenchRuntime &operator=(const WasmiBenchRuntime &) = delete;

  ~WasmiBenchRuntime()
  {
    free_wasmi_runtime(runtime_);
    free_wasmi_context(context_);
    free_wasmi_global_context(global_context_);
  }

  uint64_t invoke(std::string_view method, uint64_t gas_limit)
  {
    const WasmiInvokeResult result =
        wasmi_invoke(runtime_, reinterpret_cast<const uint8_t *>(method.data()),
                     static_cast<uint32_t>(method.size()), gas_limit);
    if (result.error != 0) {
      std::abort();
    }
    return result.gas_consumed;
  }

private:
  WasmiBenchContract &contract_;
  void *global_context_ = nullptr;
  void *context_ = nullptr;
  void *runtime_ = nullptr;
};

void
run_wasmi_benchmark(benchmark::State &state, std::vector<uint8_t> wasm)
{
  WasmiBenchContract contract{.wasm = std::move(wasm)};
  WasmiBenchRuntime runtime(contract);
  int64_t consumed_fuel = 0;

  for (auto _ : state) {
    consumed_fuel += static_cast<int64_t>(runtime.invoke("14", kBenchGasLimit));
  }

  state.SetItemsProcessed(consumed_fuel);
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
      0x00000073U,
  });

  run_rv32_benchmark(state, program);
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
      0x00000073U,
  });

  run_rv32_benchmark(state, program);
}

void
bm_rv32_fibonacci_recursive_20(benchmark::State &state)
{
  const auto program = make_recursive_fibonacci_program();
  Machine machine(program, kBenchMemoryBytes, kBenchGasLimit);
  HaltHost host;
  const Rv32RunStats stats = run_rv32_program(machine, host, kBenchGasLimit);
  if (stats.result != StepResult::kHalted ||
      machine.halt_code() != kFib20Result) {
    std::abort();
  }
  run_rv32_benchmark(state, program);
}

void
bm_wasmi_wat_fibonacci_recursive_20(benchmark::State &state)
{
  run_wasmi_benchmark(state, load_wasm_from_dir(RV32_VM_BENCH_BINARY_DIR,
                                                "rv32_fibonacci_bench.wasm"));
}

void
bm_wasmi_built_contract_fibonacci_recursive_20(benchmark::State &state)
{
  run_wasmi_benchmark(state, load_wasm_from_dir(CMAKE_ROOT_BINARY_DIR
                                                "/built_contracts",
                                                "fibonacci_raw.wasm"));
}

// Google Benchmark registers through static objects; keep the benchmark file
// tidy-clean without changing the library registration model.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
BENCHMARK(bm_dispatch_arithmetic);
BENCHMARK(bm_load_store_loop);
BENCHMARK(bm_rv32_fibonacci_recursive_20);
BENCHMARK(bm_wasmi_wat_fibonacci_recursive_20);
BENCHMARK(bm_wasmi_built_contract_fibonacci_recursive_20);
// NOLINTEND(bugprone-throwing-static-initialization)

} // namespace
} // namespace hogd::rv32

BENCHMARK_MAIN();
