#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"
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
// Signature, public key and message for VerifySignature, below the hashing
// input so the two never overlap.
// Three 64-byte signature slots need 192 bytes, so the public key sits above
// them with room to spare. The first bench-pool run put the key at 0x0980,
// exactly where the third signature lands, and the 1024-byte row verified a
// clobbered signature -- caught only because an erroring syscall fails its row
// rather than being timed. Sized from the constants rather than spelled out, so
// adding a swept length cannot silently overrun again.
constexpr uint32_t kSignatureAddress = 0x0900;
constexpr uint32_t kSignatureStride = 64;
// Message lengths VerifySignature is measured over. Each needs its OWN
// signature: a signature is over one exact byte string, so verifying a prefix
// of the signed message fails -- correctly, and on a rejection path far cheaper
// than the one being priced. Signing only the longest and sweeping lengths
// against it is what the first bench-pool run died on.
inline constexpr std::array<uint32_t, 3> kVerifyMessageBytes = {0, 64, 1024};
constexpr uint32_t kMaxMessageBytes = 1024;
// The length the ecall-crossing row uses. Mid-sweep: long enough that the
// message is not degenerate, short enough that the row still finishes.
constexpr size_t kVerifyVmRow = 1;
static_assert(kVerifyVmRow < kVerifyMessageBytes.size());

constexpr uint32_t
signature_address(size_t index)
{
  return kSignatureAddress + static_cast<uint32_t>(index) * kSignatureStride;
}

// Everything after the signatures is placed relative to them.
constexpr uint32_t kPublicKeyAddress =
    kSignatureAddress + kVerifyMessageBytes.size() * kSignatureStride;
constexpr uint32_t kPublicKeyBytes = 32;
constexpr uint32_t kMessageAddress = kPublicKeyAddress + kPublicKeyBytes;
static_assert(kMessageAddress + kMaxMessageBytes <= kInputAddress,
              "VerifySignature's material must not run into the hashing input");
// Hasher slots. HashBuffers caps live streams at the network config's
// max_sha256_buffers (8 here), and Copy needs a distinct destination, so two
// low indices are enough and stay well inside the cap.
constexpr uint32_t kHashSlot = 0;
constexpr uint32_t kHashSlotCopy = 1;

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

// The incremental hashing family, which hashes the same bytes as SHA256 and is
// charged nothing at all: syscall_crypto.cc returns WrappedGasAmount{0} from
// Update, Copy and Finalize alike. Whatever the one-shot's constants end up
// being, a guest can reach the same work through these for free, so measuring
// them is the point rather than a completeness exercise.

// Update with reset=true: creates or resets slot `index` and hashes `bytes`.
// Self-contained, so it repeats cleanly in a timing loop -- reset=false would
// append to a stream that grows without bound across iterations and stop
// measuring a fixed amount of work.
SyscallCall
sha256_update_call(uint32_t bytes)
{
  SyscallCall call = make_call("SHA256Update", SyscallNo::kSHA256Update,
                               {kHashSlot, kInputAddress, bytes, /*reset=*/1});
  call.input_bytes = bytes;
  return call;
}

// Finalize erases the stream it finalizes, so it cannot repeat on its own: the
// second call in a loop would find no stream and take an error path. The pair
// is what a guest issues anyway, and subtracting the measured Update/0 row
// gives Finalize alone -- which is why that row is swept down to zero bytes.
std::vector<SyscallCall>
sha256_finalize_pair()
{
  return {sha256_update_call(0),
          make_call("SHA256Finalize", SyscallNo::kSHA256Finalize,
                    {kHashSlot, kOutputAddressA})};
}

// Copy clones a live hasher into another slot. Needs a source stream to exist,
// which the fixture's warm-up establishes, and a destination distinct from it
// (copy() early-returns when src == dst, which would measure nothing).
SyscallCall
sha256_copy_call()
{
  return make_call("SHA256Copy", SyscallNo::kSHA256Copy,
                   {kHashSlot, kHashSlotCopy});
}

// VerifySignature(scheme, signature, public_key, message). Seven argument
// words -- the register-native ABI's maximum -- because three spans cost two
// slots each.
//
// Indexed by row rather than by length: each swept length has its own
// signature, over exactly that prefix, at its own address. Passing the length
// here instead would have to search for the matching signature, and getting
// that wrong verifies a signature against the wrong message -- which fails on
// ed25519's cheap reject path and prices the wrong thing.
SyscallCall
verify_signature_call(size_t index)
{
  const uint32_t message_bytes = kVerifyMessageBytes.at(index);
  SyscallCall call =
      make_call("VerifySignature", SyscallNo::kVerifySignature,
                {static_cast<uint32_t>(CryptographicScheme::Ed25519),
                 signature_address(index), kSignatureStride, kPublicKeyAddress,
                 kPublicKeyBytes, kMessageAddress, message_bytes});
  call.input_bytes = message_bytes;
  return call;
}

// Guest memory with the hashing input seeded, and a real ed25519 signature
// laid out where VerifySignature's arguments point.
//
// A real one, not a plausible-looking one: verification rejects a bad signature
// early, on a path far cheaper than the one being priced, and the benchmark
// would report that cheap path as the cost of verifying. The keypair is derived
// from a fixed seed so the measurement is reproducible.
std::vector<uint8_t>
make_guest_memory()
{
  // Content arbitrary but not zeros: a special-cased zero page would be
  // measuring the wrong thing. SHA256 has no such path, but the next syscall
  // added here might.
  std::vector<uint8_t> memory(kGuestMemoryBytes, 0);
  for (size_t i = kInputAddress; i < memory.size(); ++i) {
    memory[i] = static_cast<uint8_t>(i * 31U);
  }

  std::array<std::byte, 32> seed = {};
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<std::byte>(i + 1);
  }
  const auto [public_key, secret_key] = ed25519_keygen_from_seed(seed);

  std::array<uint8_t, kMaxMessageBytes> message = {};
  for (size_t i = 0; i < message.size(); ++i) {
    message[i] = static_cast<uint8_t>(i * 7U + 1U);
  }

  // One signature per swept length, each over exactly the prefix that length
  // names, so every measured call verifies rather than rejects.
  for (size_t i = 0; i < kVerifyMessageBytes.size(); ++i) {
    const auto bytes = kVerifyMessageBytes[i];
    const auto signature =
        ed25519_sign(secret_key,
                     std::as_bytes(std::span(message).first(bytes)));
    if (!signature.has_value()) {
      std::abort();
    }
    std::ranges::copy(*signature, memory.begin() + signature_address(i));
  }
  std::ranges::copy(public_key, memory.begin() + kPublicKeyAddress);
  std::ranges::copy(message, memory.begin() + kMessageAddress);
  return memory;
}

// One production syscall, called exactly as rv32_contract.cc calls it: live
// guest memory, blocking on the coroutine, nothing staged or copied.
//
// Returns the gas the syscall declared, so the benchmark can report the price
// beside the cost rather than leaving the reader to look it up.
// A syscall's result: the gas it declared, or why it did not run.
//
// Errors are reported rather than aborted. An erroring syscall took a different
// path than the one being priced -- a rejected argument, a frame that was never
// started -- so its timing must not be published either way. But aborting takes
// the whole process with it, and on the first bench-pool run one bad row
// (a VerifySignature call whose message length did not match its signature)
// killed the binary mid-write, truncated the JSON, and cost every other row in
// the run. Google Benchmark can fail one row and keep the rest, which is
// strictly better: the failure is visible, named, and contained.
struct SyscallOutcome {
  bool ok = false;
  uint64_t gas = 0;
  int error = 0;
  bool host_error = false;
};

SyscallOutcome
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
    return {.ok = false,
            .gas = gas_consumed,
            .error = static_cast<int>(host_result.error().code()),
            .host_error = true};
  }
  if (*host_result != SyscallErrorCode::kSyscallErrorCodeNone) {
    return {.ok = false,
            .gas = gas_consumed,
            .error = static_cast<int>(*host_result)};
  }
  return {.ok = true, .gas = gas_consumed};
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

// Fail one benchmark row, naming the syscall and the code, and leave the rest
// of the run alone.
void
fail_row(benchmark::State &state, const SyscallCall &call,
         const SyscallOutcome &outcome)
{
  state.SkipWithError(
      std::format("{}: {} {}", call.name,
                  outcome.host_error ? "host error" : "syscall error",
                  outcome.error));
}

void
bm_syscall_entry(benchmark::State &state, SyscallCall call)
{
  SyscallBenchFixture &fixture = shared_fixture();
  std::vector<uint8_t> memory = make_guest_memory();
  const std::span<uint8_t> memory_span(memory);

  // Warm the tip cache and any lazily built handler state before timing, so
  // the first iteration is not a different measurement from the rest -- and so
  // a syscall that cannot run fails the row here, before any timing is
  // published.
  const SyscallOutcome warm =
      invoke_syscall_entry(fixture.handler(), memory_span, call);
  if (!warm.ok) {
    fail_row(state, call, warm);
    return;
  }
  const uint64_t gas = warm.gas;

  int64_t calls = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        invoke_syscall_entry(fixture.handler(), memory_span, call).gas);
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

// Several syscalls per timed iteration, with an optional setup sequence issued
// once before timing starts.
//
// Two of the new families cannot be measured as a single repeated call.
// Finalize erases the stream it finalizes, so a second call in a loop finds
// nothing and takes an error path; the Update+Finalize pair is what a guest
// issues anyway, and subtracting the measured Update/0 row recovers Finalize
// alone. Copy is the mirror image: it needs a source stream that already
// exists, which is setup rather than per-iteration work, so seeding it inside
// the timed region would price a Copy that no guest performs.
void
bm_syscall_entry_sequence(benchmark::State &state,
                          std::vector<SyscallCall> setup,
                          std::vector<SyscallCall> timed)
{
  SyscallBenchFixture &fixture = shared_fixture();
  std::vector<uint8_t> memory = make_guest_memory();
  const std::span<uint8_t> memory_span(memory);

  const auto run_all = [&](const std::vector<SyscallCall> &calls,
                           uint64_t *gas_out) -> bool {
    for (const SyscallCall &call : calls) {
      const SyscallOutcome outcome =
          invoke_syscall_entry(fixture.handler(), memory_span, call);
      if (!outcome.ok) {
        fail_row(state, call, outcome);
        return false;
      }
      if (gas_out != nullptr) {
        *gas_out += outcome.gas;
      }
    }
    return true;
  };

  // Setup, then one warm pass of the timed sequence, then setup again to
  // restore whatever that pass consumed -- Finalize erases its stream, so the
  // warm pass leaves the handler in the state the first timed iteration
  // expects only if setup runs after it too.
  uint64_t gas = 0;
  if (!run_all(setup, nullptr) || !run_all(timed, &gas) ||
      !run_all(setup, nullptr)) {
    return;
  }

  int64_t iterations = 0;
  uint32_t input_bytes = 0;
  for (const SyscallCall &call : timed) {
    input_bytes += call.input_bytes;
  }
  for (auto _ : state) {
    for (const SyscallCall &call : timed) {
      benchmark::DoNotOptimize(
          invoke_syscall_entry(fixture.handler(), memory_span, call).gas);
    }
    benchmark::ClobberMemory();
    ++iterations;
  }

  state.SetItemsProcessed(iterations);
  if (input_bytes > 0) {
    state.SetBytesProcessed(iterations * static_cast<int64_t>(input_bytes));
  }
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
        state.SkipWithError(
            std::format("{}: host error {}", call.name,
                        static_cast<int>(host_result.error().code())));
        return;
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
      state.SkipWithError(std::format("{}: guest made {} calls, expected {}",
                                      call.name, served, kIterationsPerRun));
      return;
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

  // The incremental family. Same sweep as the one-shot up to
  // kSHA256BufferMaxSize, which is where HashBuffers::update stops accepting a
  // buffer -- so the comparison against SHA256 at the same size is direct, and
  // the point it is making is that one of the two is free.
  for (const uint32_t bytes : {0U, 32U, 256U, 1024U, 4096U}) {
    if (bytes > kSHA256BufferMaxSize) {
      continue;
    }
    benchmark::RegisterBenchmark(
        ("entry/SHA256Update/" + std::to_string(bytes)).c_str(),
        bm_syscall_entry, sha256_update_call(bytes));
  }
  benchmark::RegisterBenchmark("entry/SHA256Update0+Finalize",
                               bm_syscall_entry_sequence,
                               std::vector<SyscallCall>{},
                               sha256_finalize_pair());
  benchmark::RegisterBenchmark("entry/SHA256Copy", bm_syscall_entry_sequence,
                               std::vector<SyscallCall>{sha256_update_call(0)},
                               std::vector<SyscallCall>{sha256_copy_call()});
  benchmark::RegisterBenchmark("vm/SHA256Update/1024", bm_syscall_vm,
                               sha256_update_call(1024));

  // Signature verification. The message sweep is short because the cost is
  // dominated by the curve arithmetic rather than by hashing the message --
  // which is itself the finding, if it holds.
  for (size_t i = 0; i < kVerifyMessageBytes.size(); ++i) {
    benchmark::RegisterBenchmark(("entry/VerifySignature/" +
                                  std::to_string(kVerifyMessageBytes[i]))
                                     .c_str(),
                                 bm_syscall_entry, verify_signature_call(i));
  }
  // One length through the VM, to price the ecall crossing against a call
  // whose own cost dwarfs it.
  benchmark::RegisterBenchmark("vm/VerifySignature", bm_syscall_vm,
                               verify_signature_call(kVerifyVmRow));
}

// Everything needed to know whether this run is comparable with another one,
// and with an opcode calibration.
//
// The last part is the point. A syscall's price has to be expressed in the same
// gas unit as an instruction's, so rv32_syscall_report.py converts these
// nanoseconds through an opcode calibration's baseline -- and that is only
// legitimate if the two runs measured the same engine on the same host. Both
// binaries record the same engine fingerprint and the same build identity, so
// the report can refuse rather than silently mixing two machines' nanoseconds.
void
add_provenance_context()
{
  const rv32::gas_calibration::Provenance provenance =
      rv32::gas_calibration::collect_provenance();
  benchmark::AddCustomContext("engine_fingerprint",
                              provenance.engine_fingerprint);
  benchmark::AddCustomContext("engine_source_digest",
                              provenance.engine_source_digest);
  benchmark::AddCustomContext("hogd_host", provenance.host);
  benchmark::AddCustomContext("hogd_kernel", provenance.kernel);
  benchmark::AddCustomContext("hogd_cpu_model", provenance.cpu_model);
  benchmark::AddCustomContext("hogd_compiler", provenance.compiler);
  benchmark::AddCustomContext("hogd_build_type", provenance.build_type);
  benchmark::AddCustomContext("hogd_cxx_flags", provenance.cxx_flags);
  benchmark::AddCustomContext("hogd_source_revision",
                              provenance.source_revision);
  benchmark::AddCustomContext("hogd_production_build",
                              provenance.is_production_build() ? "true"
                                                               : "false");
}

// Refuse a non-Release build rather than warn about it.
//
// The same rule the gas calibration enforces, for the same reason: what is
// being measured is the compiled syscall path, and a Debug or ad-hoc -O2 build
// is a different one whose numbers look entirely reasonable and are silently
// about the wrong thing. A warning at the top of a benchmark run gets scrolled
// past; an exit does not. --allow-nonproduction overrides, for working on the
// benchmark itself and nothing else.
bool
refuse_nonproduction_build(bool allow)
{
  const rv32::gas_calibration::Provenance provenance =
      rv32::gas_calibration::collect_provenance();
  if (provenance.is_production_build() || allow) {
    return false;
  }
  std::println(
      stderr,
      "refusing to measure a non-Release build ({}, flags: {}).\n"
      "  Syscall costs describe the path the node runs, and that is the one "
      "the\n"
      "  project's Release configuration produces. Build with:\n"
      "    scripts/build.sh -t Release rv32_syscall_entry_bench\n"
      "  or use scripts/rv32_syscall_bench.sh, which does that for you.\n"
      "  Pass --allow-nonproduction to override for benchmark development "
      "only.",
      provenance.build_type.empty() ? "(non-CMake)"
                                    : provenance.build_type.c_str(),
      provenance.cxx_flags);
  return true;
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

  // Consumed here rather than by Google Benchmark, which would reject it.
  bool allow_nonproduction = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--allow-nonproduction") {
      allow_nonproduction = true;
      for (int j = i; j + 1 < argc; ++j) {
        argv[j] = argv[j + 1];
      }
      --argc;
      break;
    }
  }
  if (hogd::refuse_nonproduction_build(allow_nonproduction)) {
    return 2;
  }

  benchmark::Initialize(&argc, argv);
  hogd::add_provenance_context();
  hogd::register_benchmarks();
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
