#include <benchmark/benchmark.h>

#include <algorithm>
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

#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/fixtures/compiled_sha256_return/compiled_sha256_return_fixture.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/hogexec/runtime/rv32/threaded_machine.h"
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
constexpr uint32_t kArtifactReturnBufferAddress = 192;

using test_program::addi;
using test_program::branch;
using test_program::j_type;
using test_program::jalr;
using test_program::load;
using test_program::make_program;
using test_program::op;
using test_program::store;
using test_program::u_type;

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
                         ArtifactValidation validation)
{
  switch (validation) {
  case ArtifactValidation::kArithmeticControlFlow:
  {
    constexpr std::array<uint8_t, 4> kExpected = {15, 0, 0, 0};
    return memory_matches(memory, kArtifactReturnBufferAddress, kExpected);
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
    // The register-native fixture writes the digest to its output buffer (no
    // return frame to inspect); confirm the digest landed somewhere in memory.
    return std::ranges::search(memory, expected).begin() != memory.end();
  }
  }
  std::abort();
}

void
preflight_artifact_benchmark(const ArtifactImage &artifact, Host &host,
                             ArtifactValidation validation,
                             uint64_t method_id = 0)
{
  Machine machine(artifact.program, artifact.memory_bytes, kBenchGasLimit);
  if (!machine.view().write_memory(0, artifact.initial_memory)) {
    std::abort();
  }
  // v4 guest-side dispatch: seed the invoked method id into a0/a1 so the
  // guest dispatcher routes to the right entrypoint instead of trapping in
  // its default arm (mirrors rv32::RuntimeContext::invoke_internal).
  machine.seed_method_id(method_id);
  const Rv32RunStats stats =
      run_rv32_program(machine, host, *artifact.program, kBenchGasLimit);
  if (stats.result != StepResult::kHalted || machine.halt_code() != 0 ||
      !validate_artifact_memory(machine.memory(), validation)) {
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
                            ArtifactValidation validation,
                            uint64_t method_id = 0)
{
  preflight_artifact_benchmark(artifact, host, validation, method_id);
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
    machine.seed_method_id(method_id);
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

std::vector<uint32_t>
recursive_fibonacci_words()
{
  constexpr uint8_t kRa = 1;
  constexpr uint8_t kSp = 2;
  constexpr uint8_t kT0 = 5;
  constexpr uint8_t kT1 = 6;
  constexpr uint8_t kA0 = 10;

  return {
      // Entry ABI v3 seeds sp to 0; the program owns stack placement, so
      // start the stack at the top of bench memory.
      u_type(0x37, kSp, kBenchMemoryBytes),
      addi(kA0, 0, kFibInput),
      j_type(kRa, 8),
      0x00000073U,

      addi(kSp, kSp, -16),
      store(kSp, kRa, 12, 2),
      store(kSp, kA0, 8, 2),
      addi(kT0, 0, 2),
      branch(4, kA0, kT0, 36),
      addi(kA0, kA0, -1),
      j_type(kRa, -24),
      store(kSp, kA0, 4, 2),
      load(kA0, 2, kSp, 8),
      addi(kA0, kA0, -2),
      j_type(kRa, -40),
      load(kT1, 2, kSp, 4),
      op(kA0, kA0, kT1, 0, 0),
      load(kRa, 2, kSp, 12),
      addi(kSp, kSp, 16),
      jalr(0, kRa, 0),
  };
}

std::shared_ptr<const Program>
make_recursive_fibonacci_program()
{
  const std::vector<uint32_t> words = recursive_fibonacci_words();
  return make_program(std::span<const uint32_t>(words.data(), words.size()));
}

// Same fibonacci program decoded for the direct-threaded interpreter, for a
// like-for-like switch-vs-threaded comparison.
ThreadedMachinePreInstantiation
make_recursive_fibonacci_threaded_program()
{
  const std::vector<uint32_t> words = recursive_fibonacci_words();
  std::vector<uint8_t> bytes(words.size() * kInstructionBytes);
  for (size_t i = 0; i < words.size(); ++i) {
    store_u32_le(bytes, i * kInstructionBytes, words[i]);
  }
  std::vector<uint8_t> initial_memory; // all zero initial memory
  return ThreadedMachinePreInstantiation::create(bytes, initial_memory,
                                                 /*memory_start=*/0,
                                                 kBenchMemoryBytes,
                                                 /*trace=*/false)
      .value();
}

// A compute-bound counterpart to recursive fibonacci: a tight loop of
// register-to-register ALU with ZERO loads/stores. fibonacci is ~53% guest
// load/store (heavy stack spills); this is ~90% ALU + one branch per iteration,
// so it stresses the dispatch/ALU path instead of the memory-latency path.
//
//   acc = 0; for (i = 1<<15; i != 0; --i) { acc += i; <scratch ALU churn>; }
//   ecall  -> halt with acc = sum(1..32768) = 536887296
std::vector<uint32_t>
compute_loop_words()
{
  constexpr uint8_t kT0 = 5;  // loop counter
  constexpr uint8_t kT1 = 6;  // shift-amount / scratch constant
  constexpr uint8_t kT2 = 7;  // scratch (churned, result unused)
  constexpr uint8_t kT3 = 28; // scratch
  constexpr uint8_t kA0 = 10; // accumulator / halt result
  // funct3/funct7: add=(0,0) sub=(0,0x20) xor=(4,0) sll=(1,0)
  return {
      addi(kA0, 0, 0),         // acc = 0
      addi(kT0, 0, 1),         // t0 = 1
      addi(kT1, 0, 15),        // t1 = 15
      op(kT0, kT0, kT1, 1, 0), // t0 = 1 << 15 = 32768 (iteration count)
      // loop head (instruction index 4); 10-instruction body:
      op(kA0, kA0, kT0, 0, 0),    // acc += counter  (only acc update)
      op(kT2, kT2, kT0, 0, 0),    // scratch churn (no loads/stores) ...
      op(kT2, kT2, kT0, 4, 0),    // xor
      op(kT3, kT2, kT1, 1, 0),    // sll
      op(kT3, kT3, kT2, 0, 0),    // add
      op(kT3, kT3, kA0, 4, 0),    // xor
      op(kT2, kT2, kT3, 0, 0),    // add
      op(kT2, kT2, kT0, 0, 0x20), // sub
      addi(kT0, kT0, -1),         // counter--
      branch(1, kT0, 0, -36),     // bne t0, x0, loop  (back 9 instrs)
      0x00000073U,                // ecall -> halt with a0
  };
}

std::shared_ptr<const Program>
make_compute_loop_program()
{
  const std::vector<uint32_t> words = compute_loop_words();
  return make_program(std::span<const uint32_t>(words.data(), words.size()));
}

ThreadedMachinePreInstantiation
make_compute_loop_threaded_program()
{
  const std::vector<uint32_t> words = compute_loop_words();
  std::vector<uint8_t> bytes(words.size() * kInstructionBytes);
  for (size_t i = 0; i < words.size(); ++i) {
    store_u32_le(bytes, i * kInstructionBytes, words[i]);
  }
  std::vector<uint8_t> initial_memory; // all zero initial memory
  return ThreadedMachinePreInstantiation::create(bytes, initial_memory,
                                                 /*memory_start=*/0,
                                                 kBenchMemoryBytes,
                                                 /*trace=*/false)
      .value();
}

constexpr uint32_t kComputeLoopResult = 536887296; // sum(1..32768)

int64_t
compute_loop_instruction_count()
{
  const auto program = make_compute_loop_program();
  Machine machine(program, kBenchMemoryBytes, kBenchGasLimit);
  HaltHost host;
  const Rv32RunStats stats =
      run_rv32_program(machine, host, *program, kBenchGasLimit);
  if (stats.result != StepResult::kHalted ||
      machine.halt_code() != kComputeLoopResult) {
    std::abort();
  }
  return static_cast<int64_t>(stats.instructions);
}

// Independent-ILP variant: same 10-instruction loop body, but the eight adds
// target eight DIFFERENT accumulators (a0..a7), so they have no
// within-iteration dependency and their loop-carried chains run in parallel --
// maximal ILP. If this is much faster than the dependency-chain compute loop
// above, the interpreter is register-forwarding-bound (each dependent op stalls
// on the store-to-load forward through regs_), which is what register pinning
// would fix. If it's about the same, the limit is dispatch, and pinning won't
// help.
std::vector<uint32_t>
compute_loop_independent_words()
{
  constexpr uint8_t kT0 = 5; // loop counter
  constexpr uint8_t kT1 = 6; // shift-amount constant
  return {
      addi(kT0, 0, 1),         // t0 = 1
      addi(kT1, 0, 15),        // t1 = 15
      op(kT0, kT0, kT1, 1, 0), // t0 = 1 << 15 = 32768
      // loop head (index 3); eight independent accumulators a0..a7 (x10..x17),
      // each += counter; a0..a7 all start 0 from reset:
      op(10, 10, kT0, 0, 0),
      op(11, 11, kT0, 0, 0),
      op(12, 12, kT0, 0, 0),
      op(13, 13, kT0, 0, 0),
      op(14, 14, kT0, 0, 0),
      op(15, 15, kT0, 0, 0),
      op(16, 16, kT0, 0, 0),
      op(17, 17, kT0, 0, 0),
      addi(kT0, kT0, -1),     // counter--
      branch(1, kT0, 0, -36), // bne t0, x0, loop (back 9 instrs)
      0x00000073U,            // ecall -> halt with a0 = sum(1..32768)
  };
}

std::shared_ptr<const Program>
make_compute_loop_independent_program()
{
  const std::vector<uint32_t> words = compute_loop_independent_words();
  return make_program(std::span<const uint32_t>(words.data(), words.size()));
}

ThreadedMachinePreInstantiation
make_compute_loop_independent_threaded_program()
{
  const std::vector<uint32_t> words = compute_loop_independent_words();
  std::vector<uint8_t> bytes(words.size() * kInstructionBytes);
  for (size_t i = 0; i < words.size(); ++i) {
    store_u32_le(bytes, i * kInstructionBytes, words[i]);
  }
  std::vector<uint8_t> initial_memory; // all zero initial memory
  return ThreadedMachinePreInstantiation::create(bytes, initial_memory,
                                                 /*memory_start=*/0,
                                                 kBenchMemoryBytes,
                                                 /*trace=*/false)
      .value();
}

int64_t
compute_loop_independent_instruction_count()
{
  const auto program = make_compute_loop_independent_program();
  Machine machine(program, kBenchMemoryBytes, kBenchGasLimit);
  HaltHost host;
  const Rv32RunStats stats =
      run_rv32_program(machine, host, *program, kBenchGasLimit);
  if (stats.result != StepResult::kHalted ||
      machine.halt_code() != kComputeLoopResult) {
    std::abort();
  }
  return static_cast<int64_t>(stats.instructions);
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

// Executed-instruction count for the fibonacci program (same dynamic count for
// both interpreters), used to report instructions/sec.
int64_t
fibonacci_instruction_count()
{
  const auto program = make_recursive_fibonacci_program();
  Machine machine(program, kBenchMemoryBytes, kBenchGasLimit);
  HaltHost host;
  const Rv32RunStats stats =
      run_rv32_program(machine, host, *program, kBenchGasLimit);
  if (stats.result != StepResult::kHalted) {
    std::abort();
  }
  return static_cast<int64_t>(stats.instructions);
}

// Canonical switch interpreter via the production Machine::run() path (no
// per-step instrumentation), for a fair comparison against the threaded run().
void
bm_rv32_run_fibonacci_recursive_20(benchmark::State &state)
{
  const auto program = make_recursive_fibonacci_program();
  const int64_t instructions = fibonacci_instruction_count();
  Machine machine(program, kBenchMemoryBytes, kBenchGasLimit);
  HaltHost host;
  for (auto _ : state) {
    state.PauseTiming();
    machine.reset(kBenchGasLimit);
    state.ResumeTiming();
    const StepResult result = machine.run(host);
    if (result != StepResult::kHalted || machine.halt_code() != kFib20Result) {
      std::abort();
    }
    benchmark::DoNotOptimize(machine.halt_code());
  }
  state.SetItemsProcessed(state.iterations() * instructions);
}

// Direct-threaded interpreter via the untraced ThreadedMachine::run() (the
// zero-cost production path), same fibonacci program.
void
bm_rv32_threaded_fibonacci_recursive_20(benchmark::State &state)
{
  const auto program = make_recursive_fibonacci_threaded_program();
  const int64_t instructions = fibonacci_instruction_count();
  ThreadedMachine machine(program, kBenchGasLimit);
  HaltHost host;
  for (auto _ : state) {
    state.PauseTiming();
    machine.reset(kBenchGasLimit);
    state.ResumeTiming();
    const StepResult result = machine.run(host);
    if (result != StepResult::kHalted || machine.halt_code() != kFib20Result) {
      std::abort();
    }
    benchmark::DoNotOptimize(machine.halt_code());
  }
  state.SetItemsProcessed(state.iterations() * instructions);
}

// Per-invocation startup cost in isolation: machine construction (memory
// buffer setup + initial-image copy + register init) plus method-id seeding,
// no execution. This is the path the thread-local memory slot pool targets.
// Uses full-size memory (kMaxMemoryBytes, not kBenchMemoryBytes): deployed
// artifacts default to the maximum, so this is the production-shaped cost.
void
bm_rv32_threaded_instantiate(benchmark::State &state)
{
  const std::vector<uint32_t> words = recursive_fibonacci_words();
  std::vector<uint8_t> bytes(words.size() * kInstructionBytes);
  for (size_t i = 0; i < words.size(); ++i) {
    store_u32_le(bytes, i * kInstructionBytes, words[i]);
  }
  std::vector<uint8_t> initial_memory; // all zero initial memory
  const auto program =
      ThreadedMachinePreInstantiation::create(bytes, initial_memory,
                                              /*memory_start=*/0,
                                              kMaxMemoryBytes,
                                              /*trace=*/false)
          .value();
  for (auto _ : state) {
    ThreadedMachine machine(program, kBenchGasLimit);
    machine.seed_method_id(1);
    benchmark::DoNotOptimize(machine.memory().data());
  }
}

// Compute-bound (ALU/dispatch-heavy, no loads/stores) counterpart to fib, on
// the canonical switch interpreter's untraced run().
void
bm_rv32_run_compute_loop(benchmark::State &state)
{
  const auto program = make_compute_loop_program();
  const int64_t instructions = compute_loop_instruction_count();
  Machine machine(program, kBenchMemoryBytes, kBenchGasLimit);
  HaltHost host;
  for (auto _ : state) {
    state.PauseTiming();
    machine.reset(kBenchGasLimit);
    state.ResumeTiming();
    const StepResult result = machine.run(host);
    if (result != StepResult::kHalted ||
        machine.halt_code() != kComputeLoopResult) {
      std::abort();
    }
    benchmark::DoNotOptimize(machine.halt_code());
  }
  state.SetItemsProcessed(state.iterations() * instructions);
}

// Same compute-bound program on the direct-threaded interpreter.
void
bm_rv32_threaded_compute_loop(benchmark::State &state)
{
  const auto program = make_compute_loop_threaded_program();
  const int64_t instructions = compute_loop_instruction_count();
  ThreadedMachine machine(program, kBenchGasLimit);
  HaltHost host;
  for (auto _ : state) {
    state.PauseTiming();
    machine.reset(kBenchGasLimit);
    state.ResumeTiming();
    const StepResult result = machine.run(host);
    if (result != StepResult::kHalted ||
        machine.halt_code() != kComputeLoopResult) {
      std::abort();
    }
    benchmark::DoNotOptimize(machine.halt_code());
  }
  state.SetItemsProcessed(state.iterations() * instructions);
}

// Independent-ILP compute loop on the direct-threaded interpreter (vs the
// dependency-chain bm_rv32_threaded_compute_loop): dispatch-bound vs
// register-forwarding-bound probe.
void
bm_rv32_threaded_compute_loop_independent(benchmark::State &state)
{
  const auto program = make_compute_loop_independent_threaded_program();
  const int64_t instructions = compute_loop_independent_instruction_count();
  ThreadedMachine machine(program, kBenchGasLimit);
  HaltHost host;
  for (auto _ : state) {
    state.PauseTiming();
    machine.reset(kBenchGasLimit);
    state.ResumeTiming();
    const StepResult result = machine.run(host);
    if (result != StepResult::kHalted ||
        machine.halt_code() != kComputeLoopResult) {
      std::abort();
    }
    benchmark::DoNotOptimize(machine.halt_code());
  }
  state.SetItemsProcessed(state.iterations() * instructions);
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
  return parse_artifact_or_abort(
      test_artifact::build_artifact(words, initial_memory));
}

ArtifactImage
make_arithmetic_control_flow_artifact()
{
  std::vector<uint8_t> memory(256);

  return make_bench_artifact(
      {
          addi(6, 0, 0),
          addi(5, 0, 6),
          addi(5, 5, -1),
          op(6, 6, 5, 0, 0),
          branch(1, 5, 0, -8),
          addi(8, 0, kArtifactReturnBufferAddress),
          store(8, 6, 0, 2),
          // Register-native Return: a0=kReturn, a1=buffer, a2=length.
          addi(10, 0, static_cast<int32_t>(kSyscallReturn)),
          addi(11, 0, kArtifactReturnBufferAddress),
          addi(12, 0, 4),
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
          // Register-native Return: a0=kReturn, a1=buffer, a2=length.
          addi(10, 0, static_cast<int32_t>(kSyscallReturn)),
          addi(11, 0, kArtifactReturnBufferAddress),
          addi(12, 0, kCopyBytes),
          0x00000073U,
      },
      std::move(memory));
}

TrampolineResult
artifact_bench_entry(ExternHostCallContext /*context*/,
                     WasmContext wasm_context, uint32_t arg0, uint32_t arg1,
                     uint32_t arg2, uint32_t arg3, uint32_t /*arg4*/,
                     uint32_t /*arg5*/, uint32_t /*arg6*/, uint32_t /*arg7*/)
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
                  uint32_t /*arg0*/, uint32_t /*arg1*/, uint32_t /*arg2*/,
                  uint32_t /*arg3*/, uint32_t /*arg4*/, uint32_t /*arg5*/,
                  uint32_t /*arg6*/, uint32_t /*arg7*/)
{
  if (wasm_context.memory.sz > 0) {
    wasm_context.memory.mem[0] ^= 0x5aU;
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

  SyscallBridgeHost bridge(nullptr, bridge_noop_entry);
  MachineView view(0, kBenchGasLimit, registers, memory);
  // Register-native ABI: syscall number in a0, arguments in a1..a7.
  const EcallArgs args{
      .a0 = 1, .a1 = 2, .a2 = 3, .a3 = 4, .a4 = 5, .a5 = 6, .a6 = 7, .a7 = 8};
  int64_t calls = 0;
  for (auto _ : state) {
    const HostResult result = bridge.ecall(view, args);
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
  run_rv32_artifact_benchmark(
      state, artifact, host, ArtifactValidation::kCompiledSha256Return,
      fixtures::compiled_sha256_return::kSha256MethodId);
}

// Google Benchmark registers through static objects; keep the benchmark file
// tidy-clean without changing the library registration model.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
BENCHMARK(bm_dispatch_arithmetic);
BENCHMARK(bm_load_store_loop);
BENCHMARK(bm_rv32_fibonacci_recursive_20);
BENCHMARK(bm_rv32_run_fibonacci_recursive_20);
BENCHMARK(bm_rv32_threaded_fibonacci_recursive_20);
BENCHMARK(bm_rv32_threaded_instantiate);
BENCHMARK(bm_rv32_run_compute_loop);
BENCHMARK(bm_rv32_threaded_compute_loop);
BENCHMARK(bm_rv32_threaded_compute_loop_independent);
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
