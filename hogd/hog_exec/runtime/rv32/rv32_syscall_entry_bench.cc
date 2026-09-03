#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/hash_buffers.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
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
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
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
// Stateless syscalls first, and now the first stateful family: the NNInt
// storage calls, in the `frame/` and `cold/` rows. A storage syscall's cost is
// not a function of its arguments alone -- it depends on whether this
// transaction has touched the key before, how deep the invocation stack is,
// and whether the block's snapshot cache holds the key -- so those rows set
// that state up deliberately, one regime per row, rather than measuring
// whatever a warm handler happens to be in. See "Storage: the NNInt rows"
// below for what each regime is and how it is isolated.
namespace hogd {
namespace {

constexpr uint64_t kPlentyOfGas = 1'000'000'000;
// The mock serves a genesis tip at height 0, so the executing block is 1.
constexpr BlockExecutionContext kBenchBlockExecution{
    .block_height = 1,
    .block_timepoint = 1'700'000'000,
};
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
// The limit start_frame() fixes on the frame it admits: passed in rather than
// read off the db tip, so this constant does not depend on the pin.
constexpr uint32_t kMaxSha256Buffers = 8;

// NNInt argument block, above VerifySignature's material and below the hashing
// input. Every argument gets its own cache-line-sized slot so the alias guard
// never sees two of them overlap, whatever order the ABI registers them in.
constexpr uint32_t kNNIntRegionAddress = 0x0E00;
static_assert(kNNIntRegionAddress >= kMessageAddress + kMaxMessageBytes,
              "NNInt arguments must not overlap VerifySignature's message");
// StorageKeyContractData, 32 bytes. Two slots, one per syscall, so the warm
// Get and the warm SetAdd rows work different keys and neither leaves the
// other's key in a state it did not expect -- a Get after a SetAdd on the same
// key takes the cheaper written-item branch rather than the loaded one.
constexpr uint32_t kNNIntGetKeyAddress = kNNIntRegionAddress;
constexpr uint32_t kNNIntSetAddKeyAddress = kNNIntRegionAddress + 0x20;
// int128 inputs to SetAdd.
constexpr uint32_t kNNIntSetValueAddress = kNNIntRegionAddress + 0x40;
constexpr uint32_t kNNIntAddValueAddress = kNNIntRegionAddress + 0x60;
// Get's three outputs: int128, bool, int128.
constexpr uint32_t kNNIntOutSetAddress = kNNIntRegionAddress + 0x80;
constexpr uint32_t kNNIntOutSetOptAddress = kNNIntRegionAddress + 0xA0;
constexpr uint32_t kNNIntOutAddAddress = kNNIntRegionAddress + 0xC0;
static_assert(kNNIntOutAddAddress + sizeof(int128_t) <= kInputAddress,
              "NNInt arguments must not run into the hashing input");
static_assert(sizeof(StorageKeyContractData) == 32);

// Values the NNInt rows read and write. Nonzero and distinct, so a path that
// special-cased a zero or confused set with add would be measuring the wrong
// thing rather than silently agreeing.
constexpr int128_t kNNIntSnapshotValue = 7;
constexpr int128_t kNNIntSetValue = 1000;
constexpr int128_t kNNIntAddValue = 1;

// Each storage regime below reads and writes its own set of keys, so one row's
// warm-up cannot leave another row's key in a frame or in the snapshot cache.
// The tag is the key's first byte; the row's index goes in bytes 8..15.
enum class KeySpace : uint8_t {
  // The one key the warm entry/vm rows repeat on, resident in the fixture's
  // inner frame after the first call.
  kWarm = 1,
  // Keys the first-touch sweeps read, seeded in the mock db and, after each
  // row's warm pass, resident in the block's snapshot cache -- but never in a
  // frame when the timed call arrives.
  kFirstTouch = 2,
  // Keys the SetAdd sweeps write. Never seeded: a write consults neither the
  // snapshot cache nor the db.
  kSetAdd = 3,
  // Keys written in a child frame and merged into its parent on commit.
  kMerge = 4,
  // Keys used exactly once each, so every read misses the snapshot cache.
  kCold = 5,
};

StorageKeyContractData
make_nnint_key(KeySpace space, uint64_t index)
{
  StorageKeyContractData key{};
  key[0] = std::to_underlying(space);
  for (size_t i = 0; i < sizeof(index); ++i) {
    key[8 + i] = static_cast<uint8_t>(index >> (8 * i));
  }
  return key;
}

// Point the NNInt syscalls at `key` by writing it where their argument word
// says it is. A 32-byte store: in a timed loop it is ~1 ns beside a call of
// ~100, and it is what a guest does too -- its key lives in memory and it
// writes the one it wants before calling.
void
write_nnint_key(std::span<uint8_t> memory, uint32_t key_address, KeySpace space,
                uint64_t index)
{
  const StorageKeyContractData key = make_nnint_key(space, index);
  std::memcpy(memory.data() + key_address, key.data(), key.size());
}

// The most distinct keys a first-touch sweep reads. Seeded on demand rather
// than up front: every vm/ row builds its own fixture, and seeding a quarter
// of a million items per fixture is a cost only the sweeps need.
//
// The top of the sweep is not a protocol limit -- nothing caps the keys a
// transaction may touch except its gas budget -- so it is set where the
// per-key cost stops moving, which is what makes pricing at the plateau
// sound. See "How far does the key count scale" in
// docs/rv32/rv32-gas-calibration.md.
constexpr uint64_t kFirstTouchKeys = 262144;
// Keys the cold row may consume before it runs out. A cold read needs a key
// the snapshot cache has never seen, and the cache is never reset within a
// process, so each one is used exactly once. Sized for the fixed iteration
// count the row registers with, times the runner's repetitions, with room for
// the warm-up calls and a few extra repetitions. Exhausting it fails the row
// with a message rather than recycling keys and quietly measuring hits.
constexpr uint64_t kColdPoolKeys = uint64_t{1} << 18;
constexpr int64_t kColdIterations = 20000;

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
  InvokeErrorCode verify_contract(Contract const & /*contract*/,
                                  PreparedContract & /*out*/) const override
  {
    std::abort();
  }

  x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
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
    snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(0, kBenchBlockExecution,
                                               *db_proxy_);
    handler_ = std::make_unique<SystemCallHandler>(
        Transaction{}, *snapshot_cache_,
        PerTransactionDatabaseProxy(*db_proxy_, 0, 0),
        PerTransactionEventServerProxy(*event_server_proxy_, 0, 0),
        ProposalModeCtor{}, *runtime_context_);

    // Pin the db tip. Production entry paths (invoke(), root_invoke()) await
    // init_db_tip() before dispatching anything, because the pin is the
    // transaction's one fallible tip read and every later height read is then a
    // plain load. This harness calls syscall_entry() directly, so it has to do
    // the same pinning those entry paths do -- otherwise GetBlockHeight reads
    // through a null tip. Held rather than checked here: the ctor runs inside
    // the first benchmark row, so panicking in it truncates the JSON and costs
    // the whole run, which is exactly the failure this file already contains
    // per row elsewhere.
    if (auto pinned = handler_->init_db_tip()(); !pinned.has_value()) {
      setup_error_ = std::format("could not pin the db tip: system error {}",
                                 pinned.error().raw_code());
      return;
    }

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
    auto outer = push_frame(inner_address_parent());
    if (!outer.has_value()) {
      setup_error_ = frame_push_error("outer", outer.error());
      return;
    }
    outer_trace_ = std::move(outer).value();
    auto inner = push_frame(inner_address());
    if (!inner.has_value()) {
      setup_error_ = frame_push_error("inner", inner.error());
      // The outer frame is up and the dtor bails on a failed setup, so unwind
      // it here rather than leaving it open on a dead fixture.
      (void)pop_frame(std::move(outer_trace_), false);
      return;
    }
    inner_trace_ = std::move(inner).value();

    // Snapshot state the storage rows read. The mock serves whatever it was
    // handed at setup; a key it does not hold reads as kStorageKeyNotExist,
    // which fails the row rather than timing it.
    // Two warm keys: index 0 is the Get rows', index 1 the SetAdd rows'.
    seed_nnint(inner_address(), KeySpace::kWarm, 0, 2);
  }

  ~SyscallBenchFixture()
  {
    // Nothing to unwind when setup bailed: it pushed no frames, or unwound
    // the ones it had pushed on its way out.
    if (!setup_error_.empty()) {
      return;
    }
    // Completed, not discarded: finish_frame() returns a MaybeAsync, and
    // dropping one on its Async arm abandons the coroutine unrun.
    (void)handler_->finish_frame(InvokeStatus<void>{},
                                 std::move(inner_trace_))();
    (void)handler_->finish_frame(InvokeStatus<void>{},
                                 std::move(outer_trace_))();
  }

  SyscallBenchFixture(const SyscallBenchFixture &) = delete;
  SyscallBenchFixture &operator=(const SyscallBenchFixture &) = delete;
  SyscallBenchFixture(SyscallBenchFixture &&) = delete;
  SyscallBenchFixture &operator=(SyscallBenchFixture &&) = delete;

  [[nodiscard]] SystemCallHandler &handler() { return *handler_; }

  // Why the fixture is unusable, if it is. Empty when setup completed. Every
  // row checks this before touching the handler: a fixture that could not be
  // built fails all of them by name instead of aborting the process partway
  // through writing the JSON.
  [[nodiscard]] const std::string &setup_error() const { return setup_error_; }

  // Why a frame could not be pushed, named so a failed row says which one.
  static std::string frame_push_error(std::string_view which,
                                      const SystemError &error)
  {
    return std::format("could not push the {} frame: system error {}", which,
                       error.raw_code());
  }

  // Distinct, deterministic addresses so caller and callee differ.
  static ContractAddress make_address(uint8_t seed)
  {
    ContractAddress address{};
    address[0] = seed;
    return address;
  }

  // The frame the entry rows run in, and its parent.
  static ContractAddress inner_address_parent() { return make_address(1); }
  static ContractAddress inner_address() { return make_address(2); }
  // The innermost frame of every frame/ sweep, whatever its depth. A storage
  // key is namespaced by the address of the frame touching it
  // (InvocationFrameStack::get_addrkey), so the first-touch keys are seeded
  // under this one address and every sweep depth reads the same seeded set.
  static ContractAddress leaf_address() { return make_address(0x40); }
  // Intermediate frames of a depth sweep, and the merge rows' parent. Distinct
  // from the leaf, from each other, and from the fixture's own two frames,
  // because production forbids reentrance and the addresses here should not
  // describe a stack that could not exist.
  static ContractAddress spacer_address(uint32_t index)
  {
    ContractAddress address{};
    address[0] = 0x80;
    address[1] = static_cast<uint8_t>(index);
    address[2] = static_cast<uint8_t>(index >> 8);
    return address;
  }
  static ContractAddress cold_address() { return make_address(0x50); }

  // Push a frame at `address` on top of whatever is there. The trace handle
  // is what pop_frame needs back.
  //
  // Refused admission is returned, not aborted. Frame admission is a
  // production check with production reasons to say no -- the depth bound and
  // the reentrance ban both live there -- so a row that cannot build its stack
  // is a row to fail by name, exactly like a syscall that will not run. This
  // used to std::abort(), which took the process down mid-write and cost every
  // row already measured.
  [[nodiscard]] SystemStatus<std::unique_ptr<InvocationFrameTrace>>
  push_frame(ContractAddress address)
  {
    return handler_->start_frame(
        Invocation{
            .invoked_address = address,
            .entrypoint = ContractEntrypointID{},
            .gas_limit = kPlentyOfGas,
            .calldata = Calldata{},
        },
        std::to_underlying(InvokeFlag::None), std::span<const uint32_t>{},
        kMaxSha256Buffers);
  }

  // Finish the top frame. A successful child merges its modified items into
  // its parent (commit_successful_child_frame); a failed one is discarded
  // (commit_failed_child_frame), which is how a sweep row returns the stack to
  // exactly the state it found, however many keys it touched.
  // Returns why the frame could not be finished, or an empty string.
  std::string pop_frame(std::unique_ptr<InvocationFrameTrace> trace,
                        bool success)
  {
    InvokeStatus<void> result =
        success ? InvokeStatus<void>{}
                : InvokeStatus<void>{std::unexpected(
                      InvokeError{InvokeErrorCode::Deterministic})};
    // Called, not just built. finish_frame() returns a MaybeAsync and the work
    // happens when it is invoked; dropping one on its Async arm abandons the
    // coroutine with the frame still open, and has_value() on the undriven
    // MaybeAsync answers which arm it took rather than whether it succeeded.
    auto finished = handler_->finish_frame(result, std::move(trace))();
    if (!finished.has_value()) {
      return std::format("could not finish a frame: system error {}",
                         finished.error().raw_code());
    }
    return {};
  }

  // Put NNInt items under `address` in the mock snapshot, at the keys
  // make_nnint_key(space, first..last).
  void seed_nnint(ContractAddress const &address, KeySpace space,
                  uint64_t first, uint64_t last)
  {
    for (uint64_t index = first; index < last; ++index) {
      StorageKey key(StorageKeyType::ContractData);
      key.contract_data_key() = make_nnint_key(space, index);
      snapshot_server_.impl()->setup_create_item(
          AddressAndKey{.address = address, .key = key},
          StorageItemWrapper(kNNIntSnapshotValue, &cost_model()));
    }
  }

  // Seed first-touch keys up to `count`, from wherever the last call stopped.
  // The sweeps read indices 0..K-1, so a row asking for fewer than a previous
  // row needs no work.
  void ensure_first_touch_keys(uint64_t count)
  {
    if (count <= seeded_first_touch_keys_) {
      return;
    }
    seed_nnint(leaf_address(), KeySpace::kFirstTouch, seeded_first_touch_keys_,
               count);
    seeded_first_touch_keys_ = count;
  }

  // The cold row's keys, seeded on first use rather than in the constructor:
  // every vm row builds its own fixture, and a quarter-million inserts per
  // fixture is a cost nothing but the cold row needs.
  void seed_cold_pool()
  {
    if (cold_pool_seeded_) {
      return;
    }
    seed_nnint(cold_address(), KeySpace::kCold, 0, kColdPoolKeys);
    cold_pool_seeded_ = true;
  }

  // The next never-read cold key, or nullopt when the pool is spent. One
  // counter for the process: the snapshot cache is never reset, so a key read
  // by any earlier repetition is warm for every later one.
  std::optional<uint64_t> take_cold_key()
  {
    if (next_cold_key_ >= kColdPoolKeys) {
      return std::nullopt;
    }
    return next_cold_key_++;
  }

private:
  static StorageCostModel const &cost_model()
  {
    static const StorageCostModel model =
        StorageCostModel::default_cost_model();
    return model;
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
  std::string setup_error_;
  bool cold_pool_seeded_ = false;
  uint64_t next_cold_key_ = 0;
  uint64_t seeded_first_touch_keys_ = 0;
};

// One syscall's register-native arguments: a0 is the number, a1..a7 the words.
struct SyscallCall {
  std::string name;
  std::array<uint32_t, 8> words = {};
  // Bytes of guest input the call reads, for the size-swept hashing rows.
  uint32_t input_bytes = 0;
  // Where the call reads its storage key from, for the rows that change it
  // between calls. Zero for calls that take none.
  uint32_t key_address = 0;
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

SyscallCall
sha256_finalize_call(uint32_t slot)
{
  return make_call("SHA256Finalize", SyscallNo::kSHA256Finalize,
                   {slot, kOutputAddressA});
}

// Finalize erases the stream it finalizes, so it cannot repeat on its own: the
// second call in a loop would find no stream and take an error path. The pair
// is what a guest issues anyway, and subtracting the measured Update/0 row
// gives Finalize alone -- which is why that row is swept down to zero bytes.
std::vector<SyscallCall>
sha256_finalize_pair()
{
  return {sha256_update_call(0), sha256_finalize_call(kHashSlot)};
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

// The other half of Copy, and the dearer one. HashBuffers::copy takes two
// paths: a destination that already exists is an assignment over its stream,
// and one that does not is an insert into the frame's stream map -- an
// allocation. Repeating sha256_copy_call() measures only the first, because
// the warm pass creates the destination and every timed iteration after it
// finds the slot there. Finalizing the destination erases it, so pairing the
// two puts the insert back in the loop. Subtract the Finalize row to read
// Copy's allocating path alone.
//
// Which path a guest takes is its own choice, so the schedule has to hold for
// the dearer one. It charges Copy nothing beyond the crossing; this row is
// what would catch that going wrong.
std::vector<SyscallCall>
sha256_copy_new_slot_pair()
{
  return {sha256_copy_call(), sha256_finalize_call(kHashSlotCopy)};
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

// NonnegativeIntGet(key, force_load_from_snapshot) -> (set, set_opt, add).
//
// force_load_from_snapshot decides whether the handler consults the snapshot
// at all. With it set, a key this frame has not written is loaded from the
// snapshot cache (SystemCallHandler::get_nnint, the UnmodifiedItem branch);
// without it the same call answers (nullopt, 0) from the frame alone. The two
// are different code paths with different costs, and both are real: the SDK
// exposes them as get_values_force and get_values.
SyscallCall
nnint_get_call(bool force_load_from_snapshot)
{
  SyscallCall call =
      make_call(force_load_from_snapshot ? "NNIntGet" : "NNIntGetNoLoad",
                SyscallNo::kNonnegativeIntGet,
                {kNNIntGetKeyAddress, force_load_from_snapshot ? 1U : 0U,
                 kNNIntOutSetAddress, kNNIntOutSetOptAddress,
                 kNNIntOutAddAddress});
  call.key_address = kNNIntGetKeyAddress;
  return call;
}

// NonnegativeIntSetAdd(key, set_value, add_value, flags), with a set value
// present: the shape an ERC20 balance update takes after a forced read. Never
// touches the snapshot or the db -- the write is staged in the frame's item
// view and only reaches storage at commit, which is why its first-touch rows
// need no seeding and why the commit-side cost is measured separately.
SyscallCall
nnint_set_add_call()
{
  SyscallCall call = make_call("NNIntSetAdd", SyscallNo::kNonnegativeIntSetAdd,
                               {kNNIntSetAddKeyAddress, kNNIntSetValueAddress,
                                kNNIntAddValueAddress,
                                std::to_underlying(StorageDeltaFlags::None)});
  call.key_address = kNNIntSetAddKeyAddress;
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

  // The NNInt block: the warm keys, and SetAdd's two operands. The sweep rows
  // overwrite the key per call; the operands stay.
  write_nnint_key(memory, kNNIntGetKeyAddress, KeySpace::kWarm, 0);
  write_nnint_key(memory, kNNIntSetAddKeyAddress, KeySpace::kWarm, 1);
  std::memcpy(memory.data() + kNNIntSetValueAddress, &kNNIntSetValue,
              sizeof(kNNIntSetValue));
  std::memcpy(memory.data() + kNNIntAddValueAddress, &kNNIntAddValue,
              sizeof(kNNIntAddValue));
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

// The fixture, or nullptr with this row already failed. Every benchmark starts
// here: fixture construction is lazy and shared, so a setup step that could not
// complete surfaces on whichever row ran first and would otherwise take the
// process down with it.
SyscallBenchFixture *
fixture_or_fail(benchmark::State &state)
{
  SyscallBenchFixture &fixture = shared_fixture();
  if (!fixture.setup_error().empty()) {
    state.SkipWithError(fixture.setup_error());
    return nullptr;
  }
  return &fixture;
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
  SyscallBenchFixture *fixture_ptr = fixture_or_fail(state);
  if (fixture_ptr == nullptr) {
    return;
  }
  SyscallBenchFixture &fixture = *fixture_ptr;
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
  SyscallBenchFixture *fixture_ptr = fixture_or_fail(state);
  if (fixture_ptr == nullptr) {
    return;
  }
  SyscallBenchFixture &fixture = *fixture_ptr;
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

// What Copy costs as the frame's stream table fills, driven against
// HashBuffers directly rather than through the syscall path. Two reasons for
// going under the syscall: the table's size limit is max_sha256_buffers, which
// is network config -- "a choice, not a protocol default", per
// stock_network_config() -- so a chain can set it far above this repo's 8 and
// the schedule has to hold at whatever it is; and the mock snapshot server this
// fixture talks to serves the stock value, so the syscall path cannot sweep it.
// The syscall adds only the crossing on top, which is priced already.
//
// Each iteration fills a fresh table: one seeded stream, then `occupancy`
// copies into slots that do not exist yet, which is the allocating path and the
// one a contract chooses by picking fresh destination indices. Reported per
// copy, so a per-copy cost that grows with occupancy shows up as a rising
// number rather than a bigger total.
//
// Read the plateau, not the small rows. The denominator counts copies, but the
// numerator also carries the per-iteration setup this harness cannot hoist out:
// constructing the table, the seed update -- itself a slot creation -- and
// destroying every node at the end. The /0 row is exactly that setup and
// nothing else, so it is what to subtract; at occupancy 1 it is most of the
// number, and by 64 it has vanished into the noise.
void
bm_hash_buffers_fill(benchmark::State &state, uint32_t occupancy)
{
  const std::array<std::byte, 32> data{};
  int64_t copies = 0;
  for (auto _ : state) {
    HashBuffers buffers(occupancy + 1);
    auto seeded = buffers.update(0, data, /*reset=*/true);
    benchmark::DoNotOptimize(seeded);
    for (uint32_t slot = 1; slot <= occupancy; ++slot) {
      auto copied = buffers.copy(0, slot);
      benchmark::DoNotOptimize(copied);
    }
    benchmark::ClobberMemory();
    copies += occupancy;
  }
  state.SetItemsProcessed(copies);
}

// The same question on the other path that creates a slot. A reset update to an
// index with no stream emplaces exactly as copy does, and SHA256UpdateCost
// charges whole blocks only -- so a zero-byte reset update creates a stream for
// no body gas at all. If slot creation needs pricing, it needs it here too.
//
// No seed here, so its /0 row is the bare table construct and destroy, and its
// small rows are correspondingly less distorted than the copy sweep's.
void
bm_hash_buffers_update_fill(benchmark::State &state, uint32_t occupancy)
{
  const std::array<std::byte, 32> data{};
  int64_t creations = 0;
  for (auto _ : state) {
    HashBuffers buffers(occupancy + 1);
    for (uint32_t slot = 0; slot < occupancy; ++slot) {
      auto created = buffers.update(slot, data, /*reset=*/true);
      benchmark::DoNotOptimize(created);
    }
    benchmark::ClobberMemory();
    creations += occupancy;
  }
  state.SetItemsProcessed(creations);
}

// ---------------------------------------------------------------------------
// Storage: the NNInt rows
//
// The stateless rows above price a call from its arguments. A storage syscall
// cannot be priced that way, because what it does depends on state the
// arguments do not name:
//
//   - whether this transaction has already touched the key (the item view is
//     in the active frame, or in a parent frame, or nowhere yet);
//   - how deep the invocation stack is, because a key not in the active frame
//     is looked for in every parent frame before a fresh view is created
//     (InvocationFrameStack::get_item_view walks the stack on every miss);
//   - whether the block's snapshot cache holds the key, and if not, a db read.
//
// So each of those is its own regime, and each regime gets rows that put the
// handler in exactly that state before every timed call:
//
//   entry/NNIntGet            warm: the key's view is in the active frame,
//                             loaded from the snapshot. One map lookup and a
//                             variant read. entry/NNIntSetAdd is the write
//                             analogue, overwriting a view already there.
//   frame/Push/D              the baseline under every sweep: D frames
//                             pushed and popped, no syscalls. What one frame
//                             costs to open and discard.
//   frame/NNIntGetFirstTouch/D/K
//                             first touch, snapshot-cached: K distinct keys
//                             read once each in a fresh frame at depth D, so
//                             every read misses the active frame, walks D+1
//                             parents, creates a view, and loads it from the
//                             snapshot cache -- which holds every key after
//                             the row's warm pass. The frame is discarded, so
//                             the next iteration starts from the same state.
//   frame/NNIntGetFirstTouchNoLoad/1/K
//                             the same without force_load: the view is
//                             created and nothing is loaded.
//   frame/NNIntSetAddFirstTouch/1/K
//                             first-touch writes: create the view, stage the
//                             item. No snapshot, no db.
//   frame/NNIntSetAddMerge/K, frame/NNIntSetAddDiscard/K
//                             the same K writes in a child frame that then
//                             commits into a fresh parent (Merge) or is
//                             thrown away (Discard). The difference is what
//                             a successful child costs its parent per item,
//                             which is where a write's in-executor cost
//                             actually lands.
//   cold/NNIntGet             snapshot-cache miss: every call reads a key the
//                             cache has never seen, so it goes through the
//                             DatabaseProxy to the mock snapshot server. See
//                             the row for what that number is and is not.
//
// The sweeps are read the way the opcode sweeps are: a line in K, whose slope
// is the per-key cost and whose intercept must agree with frame/Push/D. A
// slope that changes with D is the parent walk; a slope that changes with K
// would be the frame's map not being O(1), which is the thing to look for
// rather than assume.
//
// What is deliberately absent: any regime that depends on the *block's*
// state rather than the transaction's. Whether a key is in the snapshot cache
// depends on which transactions ran before this one in the block, which
// validators schedule differently, so it cannot be a term in a deterministic
// price; the cold row measures it anyway, so the size of what is being
// declined is on the record.
// ---------------------------------------------------------------------------

using FrameTraces = std::vector<std::unique_ptr<InvocationFrameTrace>>;

// Push `depth` frames: spacers, then the leaf every sweep's syscalls run in.
// `traces` is reused across iterations so its allocation is not in the loop.
//
// Returns why admission refused a frame, or an empty string. Frames pushed
// before the refusal stay in `traces` for the caller to pop -- a partly built
// stack still has to come down.
std::string
push_frames(SyscallBenchFixture &fixture, uint32_t depth, FrameTraces &traces)
{
  for (uint32_t i = 0; i + 1 < depth; ++i) {
    auto spacer = fixture.push_frame(SyscallBenchFixture::spacer_address(i));
    if (!spacer.has_value()) {
      return SyscallBenchFixture::frame_push_error(std::format("spacer {}", i),
                                                   spacer.error());
    }
    traces.push_back(std::move(spacer).value());
  }
  if (depth > 0) {
    auto leaf = fixture.push_frame(SyscallBenchFixture::leaf_address());
    if (!leaf.has_value()) {
      return SyscallBenchFixture::frame_push_error("leaf", leaf.error());
    }
    traces.push_back(std::move(leaf).value());
  }
  return {};
}

// Pop everything push_frames pushed, innermost first. Every frame is failed --
// discarded, along with whatever it touched -- except the innermost when
// `commit_innermost` is set, which commits into the frame below it. That
// frame is then failed in turn, so nothing reaches the fixture's own frames.
// Returns the first failure, or an empty string. The whole stack comes down
// either way: a frame left open would corrupt every row after this one.
std::string
pop_frames(SyscallBenchFixture &fixture, FrameTraces &traces,
           bool commit_innermost = false)
{
  std::string failure;
  bool success = commit_innermost;
  while (!traces.empty()) {
    std::string popped = fixture.pop_frame(std::move(traces.back()), success);
    if (failure.empty()) {
      failure = std::move(popped);
    }
    traces.pop_back();
    success = false;
  }
  return failure;
}

void
bm_frame_push(benchmark::State &state, uint32_t depth)
{
  SyscallBenchFixture *fixture_ptr = fixture_or_fail(state);
  if (fixture_ptr == nullptr) {
    return;
  }
  SyscallBenchFixture &fixture = *fixture_ptr;
  FrameTraces traces;
  traces.reserve(depth);

  int64_t frames = 0;
  for (auto _ : state) {
    const std::string pushed = push_frames(fixture, depth, traces);
    const std::string popped = pop_frames(fixture, traces);
    if (!pushed.empty() || !popped.empty()) {
      state.SkipWithError(pushed.empty() ? popped : pushed);
      return;
    }
    benchmark::ClobberMemory();
    frames += depth;
  }
  state.SetItemsProcessed(frames);
  state.counters["depth"] = benchmark::Counter(static_cast<double>(depth));
}

// One iteration: open `depth` frames, issue `call` once per key on `keys`
// distinct keys in the innermost, close them all. Reported per iteration, so
// the report can fit the line; items_processed counts the keys.
//
// Every call is checked, not just the warm pass. A stateful row can start
// failing partway -- a key the mock was not seeded with, a frame that would
// not open -- and a row that timed the error path from then on would look
// like a fast syscall.
void
bm_frame_sweep(benchmark::State &state, uint32_t depth, uint64_t keys,
               SyscallCall call, KeySpace space, bool commit_innermost,
               bool needs_seeded_keys)
{
  SyscallBenchFixture *fixture_ptr = fixture_or_fail(state);
  if (fixture_ptr == nullptr) {
    return;
  }
  SyscallBenchFixture &fixture = *fixture_ptr;
  // Only the rows that actually read the snapshot seed it. Seeding is not
  // free to the measurement: every seeded key is a StorageItemWrapper in the
  // mock's map, and at the top of the sweep that is ~90 MB resident competing
  // for cache with the frame map being measured. A row that never consults
  // the snapshot -- the no-load reads, the writes -- would be paying for
  // scaffolding it does not use. Measured: seeding first made an unrelated
  // K=16 no-load row 2.4x dearer.
  if (needs_seeded_keys) {
    fixture.ensure_first_touch_keys(keys);
  }
  std::vector<uint8_t> memory = make_guest_memory();
  const std::span<uint8_t> memory_span(memory);
  FrameTraces traces;
  traces.reserve(depth);

  // Issues the row's calls in the frames already pushed. Returns the first
  // failure, if any, with the frames still open so the caller can close them.
  const auto run_keys = [&](uint64_t *gas_out)
      -> std::optional<std::pair<SyscallCall, SyscallOutcome>> {
    for (uint64_t index = 0; index < keys; ++index) {
      write_nnint_key(memory_span, call.key_address, space, index);
      const SyscallOutcome outcome =
          invoke_syscall_entry(fixture.handler(), memory_span, call);
      benchmark::DoNotOptimize(outcome.gas);
      if (!outcome.ok) {
        return std::make_pair(call, outcome);
      }
      if (gas_out != nullptr) {
        *gas_out = outcome.gas;
      }
    }
    return std::nullopt;
  };

  // Warm pass: faults every key into the snapshot cache (for the rows that
  // load), and fails the row here if a call cannot run. Closed with the same
  // discard the timed iterations use, so the frame state it leaves is the
  // state they start from.
  uint64_t gas = 0;
  std::string pushed = push_frames(fixture, depth, traces);
  auto failure = pushed.empty() ? run_keys(&gas) : std::nullopt;
  std::string popped = pop_frames(fixture, traces, commit_innermost);
  if (!pushed.empty() || !popped.empty()) {
    state.SkipWithError(pushed.empty() ? popped : pushed);
    return;
  }
  if (failure) {
    fail_row(state, failure->first, failure->second);
    return;
  }

  int64_t touches = 0;
  for (auto _ : state) {
    pushed = push_frames(fixture, depth, traces);
    failure = pushed.empty() ? run_keys(nullptr) : std::nullopt;
    popped = pop_frames(fixture, traces, commit_innermost);
    if (!pushed.empty() || !popped.empty()) {
      state.SkipWithError(pushed.empty() ? popped : pushed);
      return;
    }
    if (failure) {
      fail_row(state, failure->first, failure->second);
      return;
    }
    benchmark::ClobberMemory();
    touches += static_cast<int64_t>(keys);
  }

  state.SetItemsProcessed(touches);
  state.counters["depth"] = benchmark::Counter(static_cast<double>(depth));
  state.counters["keys"] = benchmark::Counter(static_cast<double>(keys));
  state.counters["gas"] = benchmark::Counter(static_cast<double>(gas));
}

// A read that misses the snapshot cache, once per call.
//
// What this measures: get_item_view's miss, the UnmodifiedItem load,
// StorageSnapshotCache::try_get_snapshot_item missing under its shared lock,
// DatabaseProxy::get_snapshot_item -- an x23 RPC, serialized, over a local
// socket, to BlockSnapshotRpc -- the SnapshotStorageItem built from the reply,
// and the cache insert. Everything on the executor's side of a cold read.
//
// What it does not measure: hog_db. The server answering is the in-process
// mock, whose lookup is an unordered_map find, so this is the fixed cost of
// going to the db and not the cost of the db. Read it as a floor under a
// cold read and as the size of the thing the schedule declines to price,
// and not as a number to put in gas_cost_table.h. The report keeps it out of
// --schedule for that reason.
//
// Keys come from a pool used strictly once: the cache is never reset within
// the process, so a key read by any earlier call -- an earlier repetition,
// the warm-up, the benchmark's own iteration-count probing -- is warm for
// every later one. Hence the fixed iteration count at registration, sized
// against kColdPoolKeys, and the loud failure when the pool runs out rather
// than a quiet slide into measuring hits.
//
// Runs in its own frame, pushed for the row and discarded after it, so the
// quarter-million item views it creates do not stay behind in the fixture's
// inner frame for every later row's lookups to walk past. Register it last:
// the snapshot cache it fills is shared, and a lookup into a cache of 250k
// entries is not the same lookup as one into a cache of 4k.
void
bm_nnint_cold(benchmark::State &state)
{
  SyscallBenchFixture *fixture_ptr = fixture_or_fail(state);
  if (fixture_ptr == nullptr) {
    return;
  }
  SyscallBenchFixture &fixture = *fixture_ptr;
  fixture.seed_cold_pool();
  std::vector<uint8_t> memory = make_guest_memory();
  const std::span<uint8_t> memory_span(memory);
  const SyscallCall call = nnint_get_call(/*force_load_from_snapshot=*/true);
  auto pushed = fixture.push_frame(SyscallBenchFixture::cold_address());
  if (!pushed.has_value()) {
    state.SkipWithError(
        SyscallBenchFixture::frame_push_error("cold", pushed.error()));
    return;
  }
  auto trace = std::move(pushed).value();

  const auto next_call = [&]() -> std::optional<SyscallOutcome> {
    const std::optional<uint64_t> index = fixture.take_cold_key();
    if (!index.has_value()) {
      state.SkipWithError(std::format(
          "cold/NNIntGet: the pool of {} never-read keys is spent; fewer "
          "repetitions, or raise kColdPoolKeys",
          kColdPoolKeys));
      return std::nullopt;
    }
    write_nnint_key(memory_span, call.key_address, KeySpace::kCold, *index);
    return invoke_syscall_entry(fixture.handler(), memory_span, call);
  };

  const std::optional<SyscallOutcome> warm = next_call();
  if (!warm.has_value()) {
    (void)fixture.pop_frame(std::move(trace), false);
    return;
  }
  if (!warm->ok) {
    fail_row(state, call, *warm);
    (void)fixture.pop_frame(std::move(trace), false);
    return;
  }

  int64_t calls = 0;
  for (auto _ : state) {
    const std::optional<SyscallOutcome> outcome = next_call();
    if (!outcome.has_value()) {
      break;
    }
    benchmark::DoNotOptimize(outcome->gas);
    if (!outcome->ok) {
      fail_row(state, call, *outcome);
      break;
    }
    benchmark::ClobberMemory();
    ++calls;
  }
  (void)fixture.pop_frame(std::move(trace), false);

  state.SetItemsProcessed(calls);
  state.counters["gas"] = benchmark::Counter(static_cast<double>(warm->gas));
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
  benchmark::RegisterBenchmark("entry/SHA256CopyNewSlot+Finalize",
                               bm_syscall_entry_sequence,
                               std::vector<SyscallCall>{sha256_update_call(0)},
                               sha256_copy_new_slot_pair());
  for (const uint32_t occupancy : {0U, 1U, 8U, 64U, 512U, 4096U, 32768U}) {
    benchmark::RegisterBenchmark(
        ("buffers/CopyFill/" + std::to_string(occupancy)).c_str(),
        bm_hash_buffers_fill, occupancy);
    benchmark::RegisterBenchmark(
        ("buffers/UpdateFill/" + std::to_string(occupancy)).c_str(),
        bm_hash_buffers_update_fill, occupancy);
  }
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
  // One length through the VM. Named with its length like every other vm row,
  // because the report pairs vm to entry by name -- an unsuffixed name matches
  // no entry row and the measurement is silently dropped from the table.
  benchmark::RegisterBenchmark(("vm/VerifySignature/" +
                                std::to_string(
                                    kVerifyMessageBytes[kVerifyVmRow]))
                                   .c_str(),
                               bm_syscall_vm,
                               verify_signature_call(kVerifyVmRow));

  // NNInt, warm: the key's view is already in the active frame. The Get rows
  // read a view loaded from the snapshot; SetAdd overwrites a staged write.
  benchmark::RegisterBenchmark("entry/NNIntGet", bm_syscall_entry,
                               nnint_get_call(true));
  benchmark::RegisterBenchmark("vm/NNIntGet", bm_syscall_vm,
                               nnint_get_call(true));
  benchmark::RegisterBenchmark("entry/NNIntGetNoLoad", bm_syscall_entry,
                               nnint_get_call(false));
  benchmark::RegisterBenchmark("entry/NNIntSetAdd", bm_syscall_entry,
                               nnint_set_add_call());
  benchmark::RegisterBenchmark("vm/NNIntSetAdd", bm_syscall_vm,
                               nnint_set_add_call());

  // The sweeps. Depth is frames on top of the fixture's two. 8 and 16 are
  // where a chain is expected to set max_cross_invoke_depth; the repo's stock
  // config admits far more, and its maximum is swept too, as the far point
  // that shows the parent walk is linear in depth rather than as a depth any
  // chain runs at. The parent walk on a first touch is linear in the stack,
  // and the schedule has to hold at whatever depth the chain allows.
  const uint32_t stock_max_depth =
      stock_network_config().max_cross_invoke_depth - 2;
  std::vector<uint32_t> depths = {1, 8, 16};
  if (std::ranges::find(depths, stock_max_depth) == depths.end()) {
    depths.push_back(stock_max_depth);
  }
  // Two key sweeps. The short one runs at every depth, where the question is
  // the parent walk. The long one runs at depth 1 only, where the question is
  // how the frame's own map scales: nothing bounds the keys a transaction may
  // touch except its gas budget, so a price taken at 4096 is only sound if the
  // cost has stopped moving by then. Registered in increasing K so the shared
  // snapshot cache grows monotonically rather than a small row inheriting a
  // large one.
  const std::array<uint64_t, 4> key_counts = {1, 16, 256, 4096};
  const std::array<uint64_t, 7> deep_key_counts = {
      1, 16, 256, 4096, 16384, 65536, kFirstTouchKeys};
  // The tail of the long sweep, split out because the loaded family runs its
  // depth axis first: see the ordering note there.
  const std::array<uint64_t, 3> deep_key_tail = {16384, 65536, kFirstTouchKeys};
  for (const uint32_t depth : depths) {
    benchmark::RegisterBenchmark(std::format("frame/Push/{}", depth),
                                 bm_frame_push, depth);
  }
  // The rows that need no seeded snapshot go first, so they measure the frame
  // map in a process the sweep has not yet grown. Registration order is run
  // order, and the seeded rows below leave ~90 MB of mock items resident.
  for (const uint64_t keys : deep_key_counts) {
    benchmark::RegisterBenchmark(
        std::format("frame/NNIntGetFirstTouchNoLoad/1/{}", keys),
        bm_frame_sweep, 1U, keys, nnint_get_call(false), KeySpace::kFirstTouch,
        /*commit_innermost=*/false, /*needs_seeded_keys=*/false);
    benchmark::RegisterBenchmark(std::format("frame/NNIntSetAddFirstTouch/1/{}",
                                             keys),
                                 bm_frame_sweep, 1U, keys, nnint_set_add_call(),
                                 KeySpace::kSetAdd,
                                 /*commit_innermost=*/false,
                                 /*needs_seeded_keys=*/false);
  }
  // The commit rows run before the seeded sweep below, not after: they
  // measure a difference of two whole-iteration timings, so a process
  // carrying the sweep's quarter-million resident items inflates both
  // sides and the per-item figure with them. Ordering them first cost
  // nothing and is what keeps that number about the merge.
  for (const uint64_t keys : key_counts) {
    // Two frames: a fresh parent for the child to commit into, so the merge
    // takes the insert path every iteration rather than the assignment path a
    // parent that already held the keys would offer.
    benchmark::RegisterBenchmark(std::format("frame/NNIntSetAddMerge/{}", keys),
                                 bm_frame_sweep, 2U, keys, nnint_set_add_call(),
                                 KeySpace::kMerge,
                                 /*commit_innermost=*/true,
                                 /*needs_seeded_keys=*/false);
    benchmark::RegisterBenchmark(std::format("frame/NNIntSetAddDiscard/{}",
                                             keys),
                                 bm_frame_sweep, 2U, keys, nnint_set_add_call(),
                                 KeySpace::kMerge,
                                 /*commit_innermost=*/false,
                                 /*needs_seeded_keys=*/false);
  }

  // The depth axis first, every depth over the same short key sweep, and only
  // then the long sweep at depth 1.
  //
  // Order matters here and getting it wrong is not visible in the numbers as a
  // failure -- it reads as a depth term that is not there. The loaded rows are
  // the only ones that fault keys into the block's snapshot cache, which is
  // shared by every row and never reset. Running depth 1 over the long sweep
  // first left a quarter-million entries in that cache for depths 8, 16 and
  // 254 to probe, and those rows then measured a cache two orders of magnitude
  // larger than the one depth 1 saw. It looked like a ~280 ns step in the
  // parent walk between depth 1 and depth 8, holding flat after; it was the
  // cache. With the depth axis run first, every depth in it sees the same
  // cache and the walk measures about 1 ns per frame at every depth.
  auto register_loaded = [](uint32_t depth, uint64_t keys) {
    benchmark::RegisterBenchmark(std::format("frame/NNIntGetFirstTouch/{}/{}",
                                             depth, keys),
                                 bm_frame_sweep, depth, keys,
                                 nnint_get_call(true), KeySpace::kFirstTouch,
                                 /*commit_innermost=*/false,
                                 /*needs_seeded_keys=*/true);
  };
  for (const uint32_t depth : depths) {
    for (const uint64_t keys : key_counts) {
      register_loaded(depth, keys);
    }
  }
  // Depth 1 only: here the question is how the frame's own map scales, not the
  // walk, so this runs last among the sweeps and takes the cache with it.
  for (const uint64_t keys : deep_key_tail) {
    register_loaded(1, keys);
  }

  // Last, because it leaves the shared snapshot cache a quarter-million
  // entries larger than every row before it found it.
  benchmark::RegisterBenchmark("cold/NNIntGet", bm_nnint_cold)
      ->Iterations(kColdIterations);
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
