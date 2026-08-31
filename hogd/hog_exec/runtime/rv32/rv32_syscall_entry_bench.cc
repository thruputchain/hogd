#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hog_exec/runtime/test_util/test_event_aggregator.h"
#include "hogd/hog_exec/shared.h"
#include "hogd/init.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/sockio.h"

// What a syscall actually costs on the production entry path.
//
// The gas calibration harness next door measures the *seam* -- the ecall
// suspend, the host dispatch, the resume -- and stops at the guest boundary,
// because it links only rv32_isa_core so it can run on a candidate validator
// without configuring the node build. This benchmark measures the other half,
// and it has to be a separate binary for exactly that reason: `syscall_entry`
// pulls in the protocol XDR, the DB proxy and the snapshot cache, which is most
// of the node.
//
// Three levels, so the two halves can be checked against the whole rather than
// assumed to compose:
//
//   entry/   syscall_entry() called directly, production-style, against live
//            guest memory. The ABI parse, the coroutine frame every call
//            allocates, and the handler. No VM.
//   vm/      the same syscall driven through ThreadedMachine exactly as
//            rv32_contract.cc drives it: ecall, suspend, syscall_entry,
//            resume. entry + seam.
//
// The difference between the two is the seam, measured here through the real
// syscall rather than a modelled host -- which is the number the calibration
// harness reports, arrived at independently.
//
// Only stateless syscalls are benchmarked. A syscall that reads storage costs
// what the snapshot cache costs, which is a different measurement with a
// different worst case (a cache miss), and pricing it needs state set up
// deliberately rather than whatever a fresh handler happens to have.
namespace hogd {
namespace {

constexpr uint64_t kPlentyOfGas = 1'000'000'000;
constexpr uint32_t kGuestMemoryBytes = rv32::kMaxMemoryBytes;
// Output pointers the guest hands the syscall. Two, because GetCalldataLen
// writes through two.
constexpr uint32_t kOutputAddressA = 0x0400;
constexpr uint32_t kOutputAddressB = 0x0800;
// Input buffer for the hashing syscalls, past both output pointers.
constexpr uint32_t kInputAddress = 0x1000;

// A RuntimeContext that can build nothing.
//
// The handler needs one to hand out child runtimes, and a stateless syscall
// never asks for a child: it answers from the frame it is already in. So rather
// than link the precompile registry (which drags in the whole built-contract
// toolchain to resolve genesis addresses this benchmark never looks up), supply
// a context whose only contract is that it is never called. If a syscall added
// here ever does invoke, this aborts rather than quietly measuring a stub.
class NoChildRuntimeContext final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const & /*contract*/) const override
  {
    std::abort();
  }

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const & /*deployed_contract*/,
                   SystemCallHandler * /*syscall_handler*/) const override
  {
    std::abort();
  }
};

// Everything a SystemCallHandler needs, and nothing else.
//
// Deliberately not NativeContractTestFramework: that stands up genesis, the
// contract loader and a real hog_db, none of which a stateless syscall touches,
// and all of which would have to be linked and paid for at startup. This is the
// same shape native_contract_test.cc uses -- an in-process snapshot server and
// event aggregator behind the ordinary proxies -- so the handler under
// measurement is the production class, constructed the production way.
class SyscallBenchFixture {
public:
  SyscallBenchFixture()
  {
    snapshot_server_.start_server(/*port=*/0);
    const auto event_exec_port = event_server_exec_server_.start_server(0);
    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = snapshot_server_.port()});
    event_server_proxy_ = std::make_unique<EventServerProxy>(
        x23::HostPort{"localhost", event_exec_port});
    event_server_.open_block(0)();
    runtime_context_ = std::make_unique<NoChildRuntimeContext>();
    snapshot_cache_ = std::make_unique<StorageSnapshotCache>(0, *db_proxy_);
    handler_ = std::make_unique<SystemCallHandler>(
        Transaction{}, *snapshot_cache_,
        PerTransactionDatabaseProxy(*db_proxy_, 0, 0),
        PerTransactionEventServerProxy(*event_server_proxy_, 0, 0),
        ProposalModeCtor{}, *runtime_context_);
    // A syscall runs inside an invocation frame; without one the handler has no
    // current address, no calldata and no gas budget, and every call under
    // measurement would take an early error path instead of doing its work.
    //
    // Two frames, not one. GetMsgSender returns
    // kSyscallErrorCodeIsRootTransaction in a root frame -- correctly, there is
    // no sender -- and an erroring syscall takes a different path than the one
    // being priced. A contract called by another contract is also the ordinary
    // case, so the inner frame is both the measurable arrangement and the
    // representative one.
    outer_trace_ = push_frame(make_address(1));
    inner_trace_ = push_frame(make_address(2));
  }

  ~SyscallBenchFixture()
  {
    (void)handler_->finish_frame(InvokeStatus<void>{}, std::move(inner_trace_));
    (void)handler_->finish_frame(InvokeStatus<void>{}, std::move(outer_trace_));
  }

  SyscallBenchFixture(const SyscallBenchFixture &) = delete;
  SyscallBenchFixture &operator=(const SyscallBenchFixture &) = delete;
  SyscallBenchFixture(SyscallBenchFixture &&) = delete;
  SyscallBenchFixture &operator=(SyscallBenchFixture &&) = delete;

  [[nodiscard]] SystemCallHandler &handler() { return *handler_; }

private:
  // Distinct, deterministic addresses so caller and callee differ.
  static ContractAddress make_address(uint8_t seed)
  {
    ContractAddress address{};
    address[0] = seed;
    return address;
  }

  std::unique_ptr<InvocationFrameTrace> push_frame(ContractAddress address)
  {
    auto stashed = handler_->start_frame(
        Invocation{
            .invoked_address = address,
            .entrypoint = ContractEntrypointID{},
            .gas_limit = kPlentyOfGas,
            .calldata = Calldata{},
        },
        std::to_underlying(InvokeFlag::None), std::span<const uint32_t>{});
    if (!stashed.has_value()) {
      std::abort();
    }
    return std::move(stashed).value();
  }

  BlockSnapshotServer snapshot_server_ = BlockSnapshotServer(/*block_id=*/0);
  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_server_exec_server_ =
      TestEventAggregatorExecServer(event_server_);
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
  std::unique_ptr<RuntimeContext> runtime_context_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;
  std::unique_ptr<SystemCallHandler> handler_;
  std::unique_ptr<InvocationFrameTrace> outer_trace_;
  std::unique_ptr<InvocationFrameTrace> inner_trace_;
};

// One syscall's register-native arguments: a0 is the number, a1..a7 the words.
struct SyscallCall {
  std::string name;
  std::array<uint32_t, 8> words = {};
  // Bytes of guest input the call reads, for the size-swept hashing rows.
  uint32_t input_bytes = 0;
};

SyscallCall
make_call(std::string name, SyscallNo number,
          std::initializer_list<uint32_t> args)
{
  SyscallCall call;
  call.name = std::move(name);
  call.words[0] = static_cast<uint32_t>(number);
  size_t i = 1;
  for (const uint32_t arg : args) {
    if (i < call.words.size()) {
      call.words[i++] = arg;
    }
  }
  return call;
}

// The stateless syscalls, in the shapes a guest actually issues them.
//
// "Stateless" here means the handler answers from the frame it is already in --
// block height, the addresses in scope, the calldata's length -- or computes
// the answer outright. No storage read, no snapshot cache lookup, no child
// invocation. Those are the ones a fixed price can describe.
std::vector<SyscallCall>
stateless_calls()
{
  return {
      // No inputs, one out pointer each. The cheapest real syscalls there are:
      // whatever these cost is very nearly the entry path's own floor.
      make_call("GetBlockHeight", SyscallNo::kGetBlockHeight,
                {kOutputAddressA}),
      make_call("GetCurrentAddress", SyscallNo::kGetCurrentAddress,
                {kOutputAddressA}),
      make_call("GetMsgSender", SyscallNo::kGetMsgSender, {kOutputAddressA}),
      make_call("GetSourceTransactionHash",
                SyscallNo::kGetSourceTransactionHash, {kOutputAddressA}),
      // Two out pointers, so the row says whether a second bounds-checked
      // output is visible against the call at all.
      make_call("GetCalldataLen", SyscallNo::kGetCalldataLen,
                {kOutputAddressA, kOutputAddressB}),
  };
}

// SHA256(buffer) -> hash. The one syscall family gas_cost_table.h actually
// prices, and the only stateless one whose cost is supposed to depend on an
// argument: SHA256HashCost is base 500 + 100/byte. A size sweep is what says
// whether that shape is right and what the two constants should be.
SyscallCall
sha256_call(uint32_t bytes)
{
  SyscallCall call = make_call("SHA256", SyscallNo::kSHA256,
                               {kInputAddress, bytes, kOutputAddressA});
  call.input_bytes = bytes;
  return call;
}

// Guest memory with the hashing input seeded. Content is arbitrary but not
// zeros: a compressor or a special-cased zero page would be measuring the
// wrong thing, and SHA256 has no such path but the next syscall might.
std::vector<uint8_t>
make_guest_memory()
{
  std::vector<uint8_t> memory(kGuestMemoryBytes, 0);
  for (size_t i = kInputAddress; i < memory.size(); ++i) {
    memory[i] = static_cast<uint8_t>(i * 31U);
  }
  return memory;
}

// One production syscall, called exactly as rv32_contract.cc calls it: live
// guest memory, blocking on the coroutine, nothing staged or copied.
//
// Returns the gas the syscall declared, so the benchmark can report the price
// beside the cost rather than leaving the reader to look it up.
uint64_t
invoke_syscall_entry(SystemCallHandler &handler, std::span<uint8_t> memory,
                     const SyscallCall &call)
{
  ExecutionContext context = {
      .memory = std::as_writable_bytes(memory),
      .gas_remaining = kPlentyOfGas,
  };
  auto [host_result, gas_consumed] =
      syscall_entry(&handler, context, call.words[0], call.words[1],
                    call.words[2], call.words[3], call.words[4], call.words[5],
                    call.words[6], call.words[7])();
  // A syscall that errored took a different path than the one being priced --
  // an unparsed argument, a frame that was never started -- and would report a
  // confident number for the wrong work.
  if (!host_result.has_value()) {
    std::println(stderr, "{}: host error {}", call.name,
                 static_cast<int>(host_result.error().code()));
    std::abort();
  }
  if (*host_result != SyscallErrorCode::kSyscallErrorCodeNone) {
    std::println(stderr, "{}: syscall error {}", call.name,
                 static_cast<int>(*host_result));
    std::abort();
  }
  return gas_consumed;
}

// One fixture for the whole process.
//
// Constructing it starts two in-process RPC servers and opens a block, which
// is slow, chatty, and -- more to the point -- irrelevant to what is being
// measured. A per-benchmark fixture also meant a fresh snapshot cache each
// time, so every run paid the cold db-tip fetch that the first GetBlockHeight
// in a block pays and the rest do not. Sharing one fixture measures the steady
// state, which is what a contract making more than one syscall actually sees.
SyscallBenchFixture &
shared_fixture()
{
  static SyscallBenchFixture fixture;
  return fixture;
}

void
bm_syscall_entry(benchmark::State &state, SyscallCall call)
{
  SyscallBenchFixture &fixture = shared_fixture();
  std::vector<uint8_t> memory = make_guest_memory();
  const std::span<uint8_t> memory_span(memory);

  // Warm the tip cache and any lazily built handler state before timing, so
  // the first iteration is not a different measurement from the rest.
  const uint64_t gas =
      invoke_syscall_entry(fixture.handler(), memory_span, call);

  int64_t calls = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        invoke_syscall_entry(fixture.handler(), memory_span, call));
    benchmark::DoNotOptimize(memory.data());
    benchmark::ClobberMemory();
    ++calls;
  }

  state.SetItemsProcessed(calls);
  if (call.input_bytes > 0) {
    state.SetBytesProcessed(calls * static_cast<int64_t>(call.input_bytes));
  }
  // The price beside the cost. A syscall charging zero direct host gas is the
  // finding rather than an omission, so the counter is emitted either way --
  // but ns/gas is only meaningful when gas is nonzero, and reporting `inf` for
  // every free syscall buries the rows where it is a real number.
  state.counters["gas"] = benchmark::Counter(static_cast<double>(gas));
}

// A guest program that issues one syscall and halts, so the same call can be
// driven end to end through the engine the node runs.
//
// The argument setup is hoisted out of the measured loop the only way a guest
// can hoist it: the registers are materialized once before the loop body, and
// the body is the ecall alone. What is timed per iteration is therefore one
// ecall plus one syscall, which is what the seam and the entry have to add up
// to.
std::vector<uint8_t>
make_ecall_program(const SyscallCall &call, uint32_t iterations)
{
  std::vector<uint32_t> words;
  // a0..a7 from the call's words. Wide values need lui+addi; test_program's
  // helpers only take a 12-bit immediate, so materialize them the long way.
  for (size_t i = 0; i < call.words.size(); ++i) {
    const auto reg = static_cast<uint8_t>(10 + i);
    const auto value = static_cast<int32_t>(call.words[i]);
    if (value >= -2048 && value <= 2047) {
      words.push_back(rv32::test_program::addi(reg, 0, value));
      continue;
    }
    const int32_t lo =
        static_cast<int32_t>(static_cast<uint32_t>(value) << 20) >> 20;
    const uint32_t hi =
        static_cast<uint32_t>(value) - static_cast<uint32_t>(lo);
    words.push_back(rv32::test_program::u_type(0x37U, reg, hi));
    words.push_back(rv32::test_program::addi(reg, reg, lo));
  }
  // Loop counter in a register the syscall ABI does not use.
  constexpr uint8_t kCounterReg = 2;
  words.push_back(rv32::test_program::addi(kCounterReg, 0,
                                           static_cast<int32_t>(iterations)));
  const auto loop_head = static_cast<int32_t>(words.size());
  words.push_back(0x00000073U); // ecall
  // a0 is clobbered by the syscall's status, so a conforming guest reloads the
  // syscall number every iteration. That one addi is inside the measured loop
  // because it is inside a real guest's loop too.
  words.push_back(
      rv32::test_program::addi(10, 0, static_cast<int32_t>(call.words[0])));
  words.push_back(rv32::test_program::addi(kCounterReg, kCounterReg, -1));
  words.push_back(rv32::test_program::branch(
      0, kCounterReg, 0, 2 * static_cast<int32_t>(rv32::kInstructionBytes)));
  const auto jal_index = static_cast<int32_t>(words.size());
  words.push_back(rv32::test_program::j_type(
      0,
      (loop_head - jal_index) * static_cast<int32_t>(rv32::kInstructionBytes)));
  // Terminal: an out-of-range syscall number, which the entry rejects without
  // doing any work, and which the driver below treats as the halt signal.
  words.push_back(rv32::test_program::addi(10, 0, 0x7FF));
  words.push_back(0x00000073U);
  return rv32::test_program::code_from_words(words);
}

void
bm_syscall_vm(benchmark::State &state, SyscallCall call)
{
  SyscallBenchFixture fixture;
  // Enough iterations per run that the program's fixed setup does not show, and
  // few enough that one run stays short next to the benchmark's own timing.
  constexpr uint32_t kIterationsPerRun = 512;
  const std::vector<uint8_t> code = make_ecall_program(call, kIterationsPerRun);
  const std::vector<uint8_t> initial_memory = make_guest_memory();
  auto program =
      rv32::ThreadedMachinePreInstantiation::create(code, initial_memory, 0,
                                                    kGuestMemoryBytes,
                                                    /*trace=*/false);
  if (!program.has_value()) {
    std::abort();
  }
  rv32::ThreadedMachine machine(*program, kPlentyOfGas);

  int64_t calls = 0;
  for (auto _ : state) {
    // Outside the timed region: restoring the 64 KiB memory image is setup,
    // not syscall cost, and it is per-run rather than per-call.
    state.PauseTiming();
    machine.reset(kPlentyOfGas);
    state.ResumeTiming();
    // The production drive loop, verbatim from rv32_contract.cc: start, then
    // syscall_entry against live machine memory on every ecall.
    rv32::StepResult result = machine.start();
    uint64_t served = 0;
    while (result == rv32::StepResult::kEcall) {
      const auto pending = machine.pending_ecall();
      ExecutionContext context = {
          .memory = std::as_writable_bytes(machine.memory()),
          .gas_remaining = machine.gas_remaining(),
      };
      auto [host_result, gas_consumed] =
          syscall_entry(&fixture.handler(), context, pending->a0, pending->a1,
                        pending->a2, pending->a3, pending->a4, pending->a5,
                        pending->a6, pending->a7)();
      if (!host_result.has_value()) {
        std::abort();
      }
      if (*host_result != SyscallErrorCode::kSyscallErrorCodeNone) {
        // The terminal call: an unknown syscall number, which is how the
        // program says it is done. Anything else is a real failure.
        break;
      }
      ++served;
      result = machine.resume_from_ecall(
          rv32::HostResult::continue_with(std::to_underlying(*host_result),
                                          gas_consumed));
    }
    if (served != kIterationsPerRun) {
      std::abort();
    }
    calls += static_cast<int64_t>(served);
  }

  state.SetItemsProcessed(calls);
  state.counters["ns_per_call"] =
      benchmark::Counter(static_cast<double>(calls),
                         benchmark::Counter::kIsRate |
                             benchmark::Counter::kInvert);
}

void
register_benchmarks()
{
  for (const SyscallCall &call : stateless_calls()) {
    benchmark::RegisterBenchmark(("entry/" + call.name).c_str(),
                                 bm_syscall_entry, call);
    benchmark::RegisterBenchmark(("vm/" + call.name).c_str(), bm_syscall_vm,
                                 call);
  }
  // SHA256 over a size sweep. 0 bytes isolates the per-call base from the
  // per-byte term, which is exactly the split SHA256HashCost claims.
  for (const uint32_t bytes : {0U, 32U, 256U, 1024U, 4096U, 16384U, 32768U}) {
    const SyscallCall call = sha256_call(bytes);
    benchmark::RegisterBenchmark(
        ("entry/SHA256/" + std::to_string(bytes)).c_str(), bm_syscall_entry,
        call);
  }
  benchmark::RegisterBenchmark("vm/SHA256/0", bm_syscall_vm, sha256_call(0));
  benchmark::RegisterBenchmark("vm/SHA256/1024", bm_syscall_vm,
                               sha256_call(1024));
}

} // namespace
} // namespace hogd

int
main(int argc, char **argv)
{
  // The same process setup every hog_exec test binary does (see
  // hogd/hog_exec/test_main.h). Not optional: the syscall handler reports
  // through the hog_exec metrics singletons, and without them the first call
  // dereferences a null registry.
  if (const int ret = hogd::hogd_init(argc, argv, false); ret != 0) {
    return ret;
  }
  hogd::init_hog_exec_metrics();

  benchmark::Initialize(&argc, argv);
  hogd::register_benchmarks();
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
