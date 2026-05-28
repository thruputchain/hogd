#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/rust/wasmi/bindings.h"

namespace hogd::rv32 {
namespace {

constexpr uint32_t kBenchMemoryBytes = 4096;
constexpr uint64_t kBenchGasLimit = 1'000'000'000;
constexpr uint32_t kFibInput = 20;
constexpr uint32_t kFib20Result = 6765;
constexpr int32_t kHostFnNoneOrRecoverable = 0;

using test_program::addi;
using test_program::branch;
using test_program::j_type;
using test_program::jalr;
using test_program::load;
using test_program::make_program;
using test_program::op;
using test_program::store;

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
  uint64_t guest_gas_consumed = 0;
  uint64_t host_gas_consumed = 0;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t branches = 0;
  uint64_t branches_taken = 0;
  uint64_t jumps = 0;
  uint64_t ecalls = 0;
  uint64_t traps = 0;
};

void
record_instruction(Rv32RunStats &stats, const DecodedInstruction &instruction,
                   uint32_t pc_before, uint32_t pc_after)
{
  ++stats.instructions;
  switch (instruction.kind) {
  case InstructionKind::kLoad:
    ++stats.loads;
    break;
  case InstructionKind::kStore:
    ++stats.stores;
    break;
  case InstructionKind::kBranch:
    ++stats.branches;
    if (pc_after != pc_before + kInstructionBytes) {
      ++stats.branches_taken;
    }
    break;
  case InstructionKind::kJal:
  case InstructionKind::kJalr:
    ++stats.jumps;
    break;
  case InstructionKind::kEcall:
    ++stats.ecalls;
    break;
  default:
    break;
  }
}

Rv32RunStats
run_rv32_program(Machine &machine, Host &host, const Program &program,
                 uint64_t gas_limit)
{
  Rv32RunStats stats;
  for (;;) {
    const uint32_t pc_before = machine.pc();
    const uint64_t gas_before = machine.gas_remaining();
    const DecodedInstruction *instruction = nullptr;
    if (program.contains_pc(pc_before)) {
      instruction = &program.instruction_at_aligned_pc(pc_before);
    }
    const StepResult result = machine.step();
    if (instruction != nullptr && machine.gas_remaining() != gas_before) {
      record_instruction(stats, *instruction, pc_before, machine.pc());
    }
    switch (result) {
    case StepResult::kContinue:
      continue;
    case StepResult::kEcall:
    {
      const auto pending_ecall = machine.pending_ecall();
      if (!pending_ecall.has_value()) {
        std::abort();
      }
      const EcallArgs ecall_args = pending_ecall.value();
      const HostResult host_result = host.ecall(machine.view(), ecall_args);
      stats.host_gas_consumed += host_result.gas_consumed;
      const StepResult completed = machine.complete_ecall(host_result);
      if (completed == StepResult::kContinue) {
        continue;
      }
      stats.result = completed;
      stats.gas_consumed = gas_limit - machine.gas_remaining();
      stats.guest_gas_consumed = stats.gas_consumed - stats.host_gas_consumed;
      if (completed == StepResult::kTrap) {
        ++stats.traps;
      }
      return stats;
    }
    case StepResult::kHalted:
    case StepResult::kTrap:
      stats.result = result;
      stats.gas_consumed = gas_limit - machine.gas_remaining();
      stats.guest_gas_consumed = stats.gas_consumed - stats.host_gas_consumed;
      if (result == StepResult::kTrap) {
        ++stats.traps;
      }
      return stats;
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
  int64_t consumed_guest_gas = 0;
  int64_t consumed_host_gas = 0;
  int64_t loads = 0;
  int64_t stores = 0;
  int64_t branches = 0;
  int64_t branches_taken = 0;
  int64_t jumps = 0;
  int64_t ecalls = 0;
  int64_t traps = 0;

  for (auto _ : state) {
    state.PauseTiming();
    machine.reset(kBenchGasLimit);
    state.ResumeTiming();

    const Rv32RunStats stats =
        run_rv32_program(machine, host, *program, kBenchGasLimit);
    if (stats.result != StepResult::kHalted) {
      std::abort();
    }
    executed_instructions += static_cast<int64_t>(stats.instructions);
    consumed_gas += static_cast<int64_t>(stats.gas_consumed);
    consumed_guest_gas += static_cast<int64_t>(stats.guest_gas_consumed);
    consumed_host_gas += static_cast<int64_t>(stats.host_gas_consumed);
    loads += static_cast<int64_t>(stats.loads);
    stores += static_cast<int64_t>(stats.stores);
    branches += static_cast<int64_t>(stats.branches);
    branches_taken += static_cast<int64_t>(stats.branches_taken);
    jumps += static_cast<int64_t>(stats.jumps);
    ecalls += static_cast<int64_t>(stats.ecalls);
    traps += static_cast<int64_t>(stats.traps);
    benchmark::DoNotOptimize(machine.halt_code());
  }

  state.SetItemsProcessed(executed_instructions);
  state.counters["gas"] =
      benchmark::Counter(static_cast<double>(consumed_gas),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.guest_gas"] =
      benchmark::Counter(static_cast<double>(consumed_guest_gas),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.host_gas"] =
      benchmark::Counter(static_cast<double>(consumed_host_gas),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.loads"] =
      benchmark::Counter(static_cast<double>(loads),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.stores"] =
      benchmark::Counter(static_cast<double>(stores),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.branches"] =
      benchmark::Counter(static_cast<double>(branches),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.branches_taken"] =
      benchmark::Counter(static_cast<double>(branches_taken),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.jumps"] =
      benchmark::Counter(static_cast<double>(jumps),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.ecalls"] =
      benchmark::Counter(static_cast<double>(ecalls),
                         benchmark::Counter::kAvgIterations);
  state.counters["rv32.traps"] =
      benchmark::Counter(static_cast<double>(traps),
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
      j_type(kRa, -24),        store(kSp, kA0, 4, 2),   load(kA0, 2, kSp, 8),
      addi(kA0, kA0, -2),      j_type(kRa, -40),        load(kT1, 2, kSp, 4),
      op(kA0, kA0, kT1, 0, 0), load(kRa, 2, kSp, 12),   addi(kSp, kSp, 16),
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
      load(3, 2, 1, 0),
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
  const Rv32RunStats stats =
      run_rv32_program(machine, host, *program, kBenchGasLimit);
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

void
store_u64_le(std::vector<uint8_t> &bytes, size_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

TrampolineResult
bridge_noop_entry(ExternHostCallContext /*context*/, WasmContext wasm_context,
                  uint64_t /*arg0*/, uint64_t /*arg1*/, uint64_t /*arg2*/,
                  uint64_t /*arg3*/, uint64_t /*arg4*/, uint64_t /*arg5*/,
                  uint64_t /*arg6*/, uint64_t /*arg7*/, uint64_t /*arg8*/,
                  uint64_t /*arg9*/, uint64_t /*arg10*/)
{
  if (wasm_context.memory.sz > kSyscallFrameBytes) {
    wasm_context.memory.mem[kSyscallFrameBytes] ^= 0x5aU;
  }
  return TrampolineResult{.result_code = 0,
                          .panic = kHostFnNoneOrRecoverable,
                          .gas_consumed = 0};
}

void
bm_syscall_bridge_roundtrip(benchmark::State &state)
{
  const auto memory_bytes = static_cast<size_t>(state.range(0));
  std::vector<uint8_t> memory(memory_bytes);
  std::array<uint32_t, kRegisterCount> registers = {};
  for (size_t i = 0; i < kSyscallFrameArgWords; ++i) {
    store_u64_le(memory, i * sizeof(uint64_t), 0x0102030405060708ULL + i);
  }

  SyscallBridgeHost bridge(nullptr, bridge_noop_entry);
  MachineView view(0, kBenchGasLimit, registers, memory);
  int64_t calls = 0;
  for (auto _ : state) {
    const HostResult result =
        bridge.ecall(view, EcallArgs{.a0 = 0, .a7 = kSyscallBridgeVersion});
    if (result.status != HostStatus::kContinue ||
        result.continue_value() != 0) {
      std::abort();
    }
    ++calls;
    benchmark::DoNotOptimize(memory.data());
  }

  state.SetItemsProcessed(calls);
  state.SetBytesProcessed(calls * static_cast<int64_t>(memory.size()));
  state.counters["rv32.bridge_memory_bytes"] =
      benchmark::Counter(static_cast<double>(memory.size()));
  state.counters["rv32.bridge_frame_bytes"] =
      benchmark::Counter(static_cast<double>(kSyscallFrameBytes));
}

// Google Benchmark registers through static objects; keep the benchmark file
// tidy-clean without changing the library registration model.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
BENCHMARK(bm_dispatch_arithmetic);
BENCHMARK(bm_load_store_loop);
BENCHMARK(bm_rv32_fibonacci_recursive_20);
BENCHMARK(bm_wasmi_wat_fibonacci_recursive_20);
BENCHMARK(bm_wasmi_built_contract_fibonacci_recursive_20);
BENCHMARK(bm_syscall_bridge_roundtrip)->Arg(4096)->Arg(65536);
// NOLINTEND(bugprone-throwing-static-initialization)

} // namespace
} // namespace hogd::rv32

BENCHMARK_MAIN();
