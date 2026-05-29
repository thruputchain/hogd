#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/dispatch.h"
#include "hogd/hogexec/runtime/rv32/fixtures/compiled_sha256_return/compiled_sha256_return_fixture.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/rust/wasmi/bindings.h"

namespace hogd::rv32 {
namespace {

constexpr uint32_t kBenchMemoryBytes = 4096;
constexpr uint64_t kBenchGasLimit = 1'000'000'000;
constexpr uint32_t kFibInput = 20;
constexpr uint32_t kFib20Result = 6765;
// Keep rv32_vm_bench independent from generated XDR headers; these values are
// pinned by the syscall/HostFn ABI exercised by the bridge benchmarks below.
constexpr int32_t kHostFnNoneOrRecoverable = 0;
constexpr int32_t kHostFnReturn = 3;
constexpr int32_t kSyscallErrorNone = 0;
constexpr int32_t kSyscallErrorFault = 1;
constexpr uint64_t kSyscallSha256 = 0;
constexpr uint64_t kSyscallReturn = 6;
constexpr size_t kSha256DigestBytes = 32;
constexpr uint32_t kArtifactFrameAddress = 64;
constexpr uint32_t kArtifactReturnBufferAddress = 192;

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

enum class ArtifactValidation {
  kArithmeticControlFlow,
  kMemoryTransform,
  kCompiledSha256Return,
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

bool
memory_matches(std::span<const uint8_t> memory, uint32_t offset,
               std::span<const uint8_t> expected)
{
  return size_t{offset} <= memory.size() &&
         expected.size() <= memory.size() - size_t{offset} &&
         std::ranges::equal(memory.subspan(offset, expected.size()), expected);
}

bool
range_fits(uint64_t offset, uint64_t length, uint64_t size)
{
  return offset <= size && length <= size - offset;
}

uint64_t
load_bench_u64_le(std::span<const uint8_t> memory, size_t offset)
{
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    value |= uint64_t{memory[offset + i]} << (i * 8);
  }
  return value;
}

std::array<uint8_t, kSha256DigestBytes>
bench_sha256_digest(std::span<const uint8_t> input)
{
  uint32_t state = 0x811c9dc5U;
  for (const uint8_t byte : input) {
    state ^= byte;
    state *= 0x01000193U;
    state = (state << 5) | (state >> 27);
  }

  std::array<uint8_t, kSha256DigestBytes> digest = {};
  for (size_t i = 0; i < digest.size(); ++i) {
    state ^= static_cast<uint32_t>(i + input.size());
    state *= 0x01000193U;
    digest[i] = static_cast<uint8_t>(state >> ((i % 4) * 8));
  }
  return digest;
}

bool
return_frame_matches(std::span<const uint8_t> memory, uint32_t frame_address,
                     std::span<const uint8_t> expected)
{
  if (!range_fits(frame_address, kSyscallFrameBytes, memory.size()) ||
      load_bench_u64_le(memory, frame_address) != kSyscallReturn ||
      load_bench_u64_le(memory, frame_address + (2 * sizeof(uint64_t))) !=
          expected.size()) {
    return false;
  }
  const uint64_t output_address =
      load_bench_u64_le(memory, frame_address + sizeof(uint64_t));
  return output_address <= std::numeric_limits<uint32_t>::max() &&
         memory_matches(memory, static_cast<uint32_t>(output_address),
                        expected);
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

bool
validate_artifact_memory(std::span<const uint8_t> memory,
                         ArtifactValidation validation,
                         uint32_t final_frame_address)
{
  switch (validation) {
  case ArtifactValidation::kArithmeticControlFlow:
  {
    constexpr std::array<uint8_t, 4> expected = {15, 0, 0, 0};
    return memory_matches(memory, kArtifactReturnBufferAddress, expected);
  }
  case ArtifactValidation::kMemoryTransform:
  {
    std::array<uint8_t, 16> expected = {};
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<uint8_t>(0x31U + i);
    }
    return memory_matches(memory, kArtifactReturnBufferAddress, expected);
  }
  case ArtifactValidation::kCompiledSha256Return:
  {
    const std::span<const uint8_t> input(
        fixtures::compiled_sha256_return::kInput);
    const std::array<uint8_t, kSha256DigestBytes> expected =
        bench_sha256_digest(input);
    return return_frame_matches(memory, final_frame_address, expected);
  }
  }
  std::abort();
}

void
preflight_artifact_benchmark(const ArtifactImage &artifact, Host &host,
                             ArtifactValidation validation)
{
  Machine machine(artifact.program, artifact.memory_bytes, kBenchGasLimit);
  if (!machine.view().write_memory(0, artifact.initial_memory)) {
    std::abort();
  }
  const Rv32RunStats stats =
      run_rv32_program(machine, host, *artifact.program, kBenchGasLimit);
  if (stats.result != StepResult::kHalted || machine.halt_code() != 0 ||
      !validate_artifact_memory(machine.memory(), validation,
                                machine.reg(10))) {
    std::abort();
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

void
run_rv32_artifact_benchmark(benchmark::State &state,
                            const ArtifactImage &artifact, Host &host,
                            ArtifactValidation validation)
{
  preflight_artifact_benchmark(artifact, host, validation);
  Machine machine(artifact.program, artifact.memory_bytes, kBenchGasLimit);
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
    if (!machine.view().write_memory(0, artifact.initial_memory)) {
      std::abort();
    }
    state.ResumeTiming();

    const Rv32RunStats stats =
        run_rv32_program(machine, host, *artifact.program, kBenchGasLimit);
    if (stats.result != StepResult::kHalted || machine.halt_code() != 0) {
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

void
write_bench_syscall_frame(std::vector<uint8_t> &memory, uint32_t frame_address,
                          std::initializer_list<uint64_t> words)
{
  size_t i = 0;
  for (const uint64_t word : words) {
    store_u64_le(memory, frame_address + (i * sizeof(uint64_t)), word);
    ++i;
  }
}

ArtifactImage
parse_artifact_or_abort(std::span<const uint8_t> artifact)
{
  auto parsed = parse_artifact(artifact);
  if (!parsed.has_value()) {
    std::abort();
  }
  return std::move(*parsed);
}

ArtifactImage
make_bench_artifact(std::initializer_list<uint32_t> words,
                    std::vector<uint8_t> initial_memory)
{
  const std::array<DispatchEntry, 1> dispatch = {{{.method_id = 0, .pc = 0}}};
  return parse_artifact_or_abort(
      test_artifact::build_artifact(words, initial_memory, dispatch));
}

ArtifactImage
make_arithmetic_control_flow_artifact()
{
  std::vector<uint8_t> memory(256);
  write_bench_syscall_frame(memory, kArtifactFrameAddress,
                            {kSyscallReturn, kArtifactReturnBufferAddress, 4, 0,
                             0, 0});

  return make_bench_artifact(
      {
          addi(6, 0, 0),
          addi(5, 0, 6),
          addi(5, 5, -1),
          op(6, 6, 5, 0, 0),
          branch(1, 5, 0, -8),
          addi(8, 0, kArtifactReturnBufferAddress),
          store(8, 6, 0, 2),
          addi(10, 0, kArtifactFrameAddress),
          addi(17, 0, kSyscallBridgeVersion),
          0x00000073U,
      },
      std::move(memory));
}

ArtifactImage
make_memory_transform_artifact()
{
  constexpr uint32_t kSourceAddress = 224;
  constexpr uint32_t kCopyBytes = 16;
  std::vector<uint8_t> memory(256);
  write_bench_syscall_frame(memory, kArtifactFrameAddress,
                            {kSyscallReturn, kArtifactReturnBufferAddress,
                             kCopyBytes, 0, 0, 0});
  for (uint32_t i = 0; i < kCopyBytes; ++i) {
    memory[kSourceAddress + i] = static_cast<uint8_t>(0x30U + i);
  }

  return make_bench_artifact(
      {
          addi(1, 0, kSourceAddress),
          addi(2, 0, kArtifactReturnBufferAddress),
          addi(3, 0, kCopyBytes),
          load(4, 0, 1, 0),
          addi(4, 4, 1),
          store(2, 4, 0, 0),
          addi(1, 1, 1),
          addi(2, 2, 1),
          addi(3, 3, -1),
          branch(1, 3, 0, -24),
          addi(10, 0, kArtifactFrameAddress),
          addi(17, 0, kSyscallBridgeVersion),
          0x00000073U,
      },
      std::move(memory));
}

TrampolineResult
artifact_bench_entry(ExternHostCallContext /*context*/,
                     WasmContext wasm_context, uint64_t arg0, uint64_t arg1,
                     uint64_t arg2, uint64_t arg3, uint64_t /*arg4*/,
                     uint64_t /*arg5*/, uint64_t /*arg6*/, uint64_t /*arg7*/)
{
  if (arg0 == kSyscallReturn) {
    return TrampolineResult{.result_code = 0,
                            .panic = kHostFnReturn,
                            .gas_consumed = 4};
  }
  if (arg0 == kSyscallSha256) {
    if (!range_fits(arg1, arg2, wasm_context.memory.sz) ||
        !range_fits(arg3, kSha256DigestBytes, wasm_context.memory.sz)) {
      return TrampolineResult{.result_code = kSyscallErrorFault,
                              .panic = kHostFnNoneOrRecoverable,
                              .gas_consumed = 4};
    }
    const auto input_offset = static_cast<size_t>(arg1);
    const auto input_size = static_cast<size_t>(arg2);
    const auto output_offset = static_cast<size_t>(arg3);
    const auto digest = bench_sha256_digest(
        std::span<const uint8_t>(wasm_context.memory.mem + input_offset,
                                 input_size));
    for (size_t i = 0; i < kSha256DigestBytes; ++i) {
      wasm_context.memory.mem[output_offset + i] = digest[i];
    }
    return TrampolineResult{.result_code = kSyscallErrorNone,
                            .panic = kHostFnNoneOrRecoverable,
                            .gas_consumed = 4};
  }
  return TrampolineResult{.result_code = kSyscallErrorFault,
                          .panic = kHostFnNoneOrRecoverable,
                          .gas_consumed = 4};
}

TrampolineResult
bridge_noop_entry(ExternHostCallContext /*context*/, WasmContext wasm_context,
                  uint64_t /*arg0*/, uint64_t /*arg1*/, uint64_t /*arg2*/,
                  uint64_t /*arg3*/, uint64_t /*arg4*/, uint64_t /*arg5*/,
                  uint64_t /*arg6*/, uint64_t /*arg7*/)
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

void
bm_rv32_artifact_arithmetic_control_flow(benchmark::State &state)
{
  const ArtifactImage artifact = make_arithmetic_control_flow_artifact();
  SyscallBridgeHost host(nullptr, &artifact_bench_entry);
  run_rv32_artifact_benchmark(state, artifact, host,
                              ArtifactValidation::kArithmeticControlFlow);
}

void
bm_rv32_artifact_memory_transform(benchmark::State &state)
{
  const ArtifactImage artifact = make_memory_transform_artifact();
  SyscallBridgeHost host(nullptr, &artifact_bench_entry);
  run_rv32_artifact_benchmark(state, artifact, host,
                              ArtifactValidation::kMemoryTransform);
}

void
bm_rv32_artifact_compiled_sha256_return(benchmark::State &state)
{
  const ArtifactImage artifact =
      parse_artifact_or_abort(fixtures::compiled_sha256_return::kArtifact);
  SyscallBridgeHost host(nullptr, &artifact_bench_entry);
  run_rv32_artifact_benchmark(state, artifact, host,
                              ArtifactValidation::kCompiledSha256Return);
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
BENCHMARK(bm_rv32_artifact_arithmetic_control_flow);
BENCHMARK(bm_rv32_artifact_memory_transform);
BENCHMARK(bm_rv32_artifact_compiled_sha256_return);
// NOLINTEND(bugprone-throwing-static-initialization)

} // namespace
} // namespace hogd::rv32

BENCHMARK_MAIN();
