#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
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

#include "db_conflict.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/mldsa_signer.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/hash_buffers.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/test_util/test_event_aggregator.h"
#include "hogd/hog_exec/shared.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "network_config.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/sockio.h"

// Production syscall measurements. entry/ calls syscall_entry directly;
// vm/ includes guest argument setup, ECALL, and resume. Their difference
// cross-checks the synthetic seam in gas_calibration/, with per-call setup
// included. frame/, buffers/, and cold/ cover stateful paths hidden by warm
// repetition. See gas_calibration/README.md for method and limitations.
namespace hogd {
namespace {

constexpr uint64_t kPlentyOfGas = 1'000'000'000;
constexpr uint32_t kGuestMemoryBytes = rv32::kDefaultMaxMemoryBytes;
// Output pointers the guest hands the syscall. Two, because GetCalldataLen
// writes through two.
constexpr uint32_t kOutputAddressA = 0x0400;
constexpr uint32_t kOutputAddressB = 0x0800;
// Input buffer for the hashing syscalls, past both output pointers.
constexpr uint32_t kInputAddress = 0x1000;
// Place each signature and its public key/message below the hashing input.
// Derive addresses from the swept lengths to prevent overlap.
constexpr uint32_t kSignatureAddress = 0x0900;
constexpr uint32_t kSignatureStride = 64;
// Each message length needs its own valid signature; a mismatched length
// measures rejection instead of successful verification.
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
// max_sha256_buffers, and Copy needs a distinct destination, so two
// low indices are enough and stay well inside the cap.
constexpr uint32_t kHashSlot = 0;
constexpr uint32_t kHashSlotCopy = 1;
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

// Bytestring argument block, above the NNInt one. Two keys, so the Get rows and
// the Set rows work separate items: a Get after a Set on one key reads the
// staged write rather than the loaded view, which is a different path than the
// one the Get rows are pricing.
constexpr uint32_t kBytestringRegionAddress = kNNIntRegionAddress + 0xE0;
static_assert(kBytestringRegionAddress >=
                  kNNIntOutAddAddress + sizeof(int128_t),
              "bytestring arguments must not overlap the NNInt block");
constexpr uint32_t kBytestringGetKeyAddress = kBytestringRegionAddress;
constexpr uint32_t kBytestringSetKeyAddress = kBytestringRegionAddress + 0x20;
constexpr uint32_t kBytestringWrittenLenAddress =
    kBytestringRegionAddress + 0x40;
static_assert(kBytestringWrittenLenAddress + sizeof(RuntimeMemoryLength) <=
                  kInputAddress,
              "bytestring arguments must not run into the payload buffer");

// The value a Set writes and the buffer a Get fills, both at the hashing
// input's address: no row issues a hash and a bytestring call, and the region
// is already seeded with a nonzero pattern.
constexpr uint32_t kBytestringPayloadAddress = kInputAddress;
// Use the hashing length sweep for copy/hash comparisons. The payload and
// argument blocks must fit within this fixture's default-size guest memory;
// the sweep does not reach the bytestring protocol ceiling.
inline constexpr std::array<uint32_t, 7> kBytestringBytes = {0,    32,   256,
                                                             1024, 4096, 16384,
                                                             32768};
constexpr uint32_t kMaxBytestringBytes = kBytestringBytes.back();
// The length the ecall-crossing rows use: the stock configured limit, so the
// one length driven end to end is the largest a chain running this repo's
// parameters would actually admit.
constexpr size_t kBytestringVmRow = 3;
static_assert(kBytestringVmRow < kBytestringBytes.size());
static_assert(kBytestringBytes[kBytestringVmRow] == kDefaultMaxBytestringLen);
static_assert(kBytestringPayloadAddress + kMaxBytestringBytes <=
                  kGuestMemoryBytes,
              "the bytestring payload must fit in guest memory");
static_assert(kMaxBytestringBytes <= kBytestringCeilingLen,
              "the sweep must stay inside the wire format's ceiling");

// ML-DSA-65 VerifySignature material, past the largest hashing and bytestring
// input: its signatures and key are too large for the low block ed25519 uses.
// One signature per swept message length, over the same message.
constexpr uint32_t kMlDsaSignatureAddress =
    kBytestringPayloadAddress + kMaxBytestringBytes;
constexpr uint32_t kMlDsaSignatureStride = 3312;
static_assert(kMlDsaSignatureStride >= sizeof(MlDsa65Signature));

constexpr uint32_t
mldsa_signature_address(size_t index)
{
  return kMlDsaSignatureAddress +
         (static_cast<uint32_t>(index) * kMlDsaSignatureStride);
}

constexpr uint32_t kMlDsaPublicKeyAddress =
    kMlDsaSignatureAddress +
    (kVerifyMessageBytes.size() * kMlDsaSignatureStride);
static_assert(kMlDsaPublicKeyAddress + sizeof(MlDsa65PublicKey) <=
                  kGuestMemoryBytes,
              "the ML-DSA material must fit in guest memory");

// The cross-invoke depth the fixture admits, and so the deepest parent walk
// the frame sweeps can measure. The stock chain caps a transaction at
// kDefaultMaxCrossInvokeDepth frames, but the walk's per-frame cost is a
// property of the frame map and not of what a chain admits, and the committed
// syscall_reference.txt guards it at depth 254 -- measured when the stock
// config still stated 256. Pinning the floor here keeps the deepest row where
// the reference expects it whatever the chain's default is, so lowering that
// default is not reported as a cost that stopped being measured. A chain that
// admits more than this still gets its full walk swept.
constexpr uint32_t kBenchMinCrossInvokeDepth = 256;

uint32_t
bench_max_cross_invoke_depth()
{
  return std::max(
      kBenchMinCrossInvokeDepth,
      NetworkConfigParams(stock_network_config().v0()).max_cross_invoke_depth);
}

// Raise the fixture's bytestring limit to cover the sweep. Otherwise the large
// write rows would time input rejection instead of copying. The cross-invoke
// depth is raised for the same reason: under the stock limit the deep frame
// sweeps would time admission refusing a frame instead of the walk.
NetworkConfig
bench_network_config()
{
  NetworkConfig config = stock_network_config();
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxBytestringLen,
                                 kMaxBytestringBytes);
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxCrossInvokeDepth,
                                 bench_max_cross_invoke_depth());
  return config;
}

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
  // Seeded first-touch keys become snapshot-cached during warmup. Each timed
  // iteration uses a fresh frame, so its item-view lookup still misses.
  kFirstTouch = 2,
  // Keys the SetAdd sweeps write. Never seeded: a write consults neither the
  // snapshot cache nor the db.
  kSetAdd = 3,
  // Keys written in a child frame and merged into its parent on commit.
  kMerge = 4,
  // Keys used exactly once each, so every read misses the snapshot cache.
  kCold = 5,
  // Keys holding one seeded bytestring per swept length. The index is the
  // length, so a row's key names the item it reads.
  kBytestring = 6,
  // Keys the bytestring write rows target. Never seeded: a write consults
  // neither the snapshot cache nor the db.
  kBytestringSet = 7,
};

StorageKeyContractData
make_item_key(KeySpace space, uint64_t index)
{
  StorageKeyContractData key{};
  key[0] = std::to_underlying(space);
  for (size_t i = 0; i < sizeof(index); ++i) {
    key[8 + i] = static_cast<uint8_t>(index >> (8 * i));
  }
  return key;
}

// Write the selected key into guest memory. This copy remains in the timed
// first-touch loop along with the syscall.
void
write_item_key(std::span<uint8_t> memory, uint32_t key_address, KeySpace space,
               uint64_t index)
{
  const StorageKeyContractData key = make_item_key(space, index);
  std::memcpy(memory.data() + key_address, key.data(), key.size());
}

// First-touch sweep limit, not a protocol cap. Seed on demand to avoid adding
// the large fixture working set to rows that do not need it. See
// gas_calibration/MEASUREMENTS.md for the measured occupancy dependence.
constexpr uint64_t kFirstTouchKeys = 262144;
// Cold keys are single-use across warmup, probing, and repetitions because the
// snapshot cache persists. Exhaustion fails the row instead of measuring hits.
constexpr uint64_t kColdPoolKeys = uint64_t{1} << 18;
// Heap for the bench's hog_db. The cold pool alone is a quarter-million NNInt
// items, each carrying its key, and ramdb's allocator fails a write rather
// than growing, so this is sized well past what the pool needs -- an
// over-estimate costs address space in one benchmark process and an
// under-estimate fails the row.
constexpr size_t kHogDbHeapBytes = size_t{1} << 30;
constexpr int64_t kColdIterations = 20000;

// These benchmarks do not invoke child contracts. Abort an unexpected build
// request so a new benchmark cannot silently measure a stub runtime.
class NoChildRuntimeContext final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(
      Contract const & /*contract*/, NetworkConfigParams const & /*params*/,
      PreparedContract & /*out*/,
      std::optional<ContractRejectCode> & /*rejection*/) const override
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

// NativeContractTestFramework starts a real hog_db and opens a block. Warm
// rows use frame/cache state; the cold row reaches the database while timed.
class SyscallBenchFixture {
public:
  SyscallBenchFixture()
  {

    // The db tip is loaded by make_new_system_call_handler() when handler_ is
    // initialized, so GetBlockHeight and its kin read a pinned tip with no
    // per-fixture step here.

    // Use a child frame so address, calldata, and gas state exist and
    // GetMsgSender succeeds; the root frame has no sender.
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

    // Seed separate keys for warm Get and SetAdd rows. Missing keys would
    // measure a different result path.
    seed_nnint(inner_address(), KeySpace::kWarm, 0, 2);
    // One item per swept length. ~54 KB in total, so every fixture can carry
    // them rather than seeding them on demand as the key sweeps do.
    seed_bytestrings(inner_address());
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
    (void)handler_.finish_frame(InvokeStatus<void>{},
                                std::move(inner_trace_))();
    (void)handler_.finish_frame(InvokeStatus<void>{},
                                std::move(outer_trace_))();
  }

  SyscallBenchFixture(const SyscallBenchFixture &) = delete;
  SyscallBenchFixture &operator=(const SyscallBenchFixture &) = delete;
  SyscallBenchFixture(SyscallBenchFixture &&) = delete;
  SyscallBenchFixture &operator=(SyscallBenchFixture &&) = delete;

  [[nodiscard]] SystemCallHandler &handler() { return handler_; }

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

  // Return admission failures to the benchmark so one invalid depth or
  // reentrance case fails its row without truncating the whole JSON output.
  [[nodiscard]] SystemStatus<std::unique_ptr<InvocationFrameTrace>>
  push_frame(ContractAddress address)
  {
    return handler_.start_frame(
        Invocation{
            .invoked_address = address,
            .entrypoint = ContractEntrypointID{},
            .gas_limit = kPlentyOfGas,
            .calldata = Calldata{},
        },
        std::to_underlying(InvokeFlag::None), ValidatedAuthTokens{});
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
    auto finished = handler_.finish_frame(result, std::move(trace))();
    if (!finished.has_value()) {
      return std::format("could not finish a frame: system error {}",
                         finished.error().raw_code());
    }
    return {};
  }

  // Seed NNInt items under address at keys make_item_key(space, first..last).
  void seed_nnint(ContractAddress const &address, KeySpace space,
                  uint64_t first, uint64_t last)
  {
    for (uint64_t index = first; index < last; ++index) {
      StorageKey key(StorageKeyType::ContractData);
      key.contract_data_key() = make_item_key(space, index);
      seed_item(AddressAndKey{.address = address, .key = key},
                StorageItemWrapper(kNNIntSnapshotValue, &cost_model()));
    }
  }

  // Put one bytestring per swept length under `address`, at the key that names
  // its length. The Get rows read these; the Set rows write keys of their own
  // and need no seeding.
  void seed_bytestrings(ContractAddress const &address)
  {
    for (const uint32_t bytes : kBytestringBytes) {
      StorageKey key(StorageKeyType::ContractData);
      key.contract_data_key() = make_item_key(KeySpace::kBytestring, bytes);
      Bytestring value;
      value.resize_uninitialized(bytes);
      // Not zeros: a path that special-cased an empty or all-zero value would
      // be measuring something other than the copy it is meant to.
      std::ranges::fill(value, uint8_t{0xAB});
      seed_item(AddressAndKey{.address = address, .key = key},
                StorageItemWrapper(std::move(value), &cost_model()));
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

  // Defer the large cold-key population until its row needs it.
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

  void seed_item(AddressAndKey const &addrkey, StorageItemWrapper item)
  {
    framework_.write_item(addrkey, std::make_shared<const StorageItemWrapper>(
                                       std::move(item)));
  }

  NativeContractTestFramework framework_{
      /*log=*/false,
      BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB::kHogDb,
      /*empty_genesis=*/true,
      /*genesis_cost_model=*/std::nullopt,
      bench_network_config(),
      kBulkRpcMaxMessageSize,
      kHogDbHeapBytes,
  };
  Transaction tx_{};
  // Built in place: SystemCallHandler is neither movable nor copyable. Declared
  // after what it borrows.
  std::unique_ptr<RuntimeContext> runtime_context_ =
      std::make_unique<NoChildRuntimeContext>();
  SystemCallHandler handler_ =
      framework_.make_new_system_call_handler(tx_, *runtime_context_);
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
  // Bytes of the guest buffer the call reads or fills, for the size-swept
  // rows.
  uint32_t payload_bytes = 0;
  // Where the call reads its storage key from, for the rows that change it
  // between calls. Zero for calls that take none.
  uint32_t key_address = 0;
  // The key this row places at key_address before it runs, for rows whose key
  // names the item they work. Absent for calls whose key make_guest_memory
  // already places, and for calls that take none.
  std::optional<std::pair<KeySpace, uint64_t>> row_key;
};

// Place a row's key where its call expects to read it. A no-op for the rows
// whose key make_guest_memory already wrote.
void
apply_row_key(std::span<uint8_t> memory, const SyscallCall &call)
{
  if (call.row_key.has_value()) {
    write_item_key(memory, call.key_address, call.row_key->first,
                   call.row_key->second);
  }
}

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

// Frame-local reads: no storage lookup or child invocation.
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

// Sweep SHA256 input size to check the compression-block cost model.
SyscallCall
sha256_call(uint32_t bytes)
{
  SyscallCall call = make_call("SHA256", SyscallNo::kSHA256,
                               {kInputAddress, bytes, kOutputAddressA});
  call.payload_bytes = bytes;
  return call;
}

// Update with reset=true: creates or resets slot `index` and hashes `bytes`.
// Self-contained, so it repeats cleanly in a timing loop -- reset=false would
// append to a stream that grows without bound across iterations and stop
// measuring a fixed amount of work.
SyscallCall
sha256_update_call(uint32_t bytes)
{
  SyscallCall call = make_call("SHA256Update", SyscallNo::kSHA256Update,
                               {kHashSlot, kInputAddress, bytes, /*reset=*/1});
  call.payload_bytes = bytes;
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

// Finalizing the destination erases it after each Copy, forcing allocation
// on the next iteration. Subtract Finalize to estimate Copy's allocating path;
// the warm Copy row reuses its destination.
std::vector<SyscallCall>
sha256_copy_new_slot_pair()
{
  return {sha256_copy_call(), sha256_finalize_call(kHashSlotCopy)};
}

// Index selects a message length and its matching valid signature. The three
// spans plus scheme consume all seven syscall argument words.
SyscallCall
verify_signature_call(size_t index)
{
  const uint32_t message_bytes = kVerifyMessageBytes.at(index);
  SyscallCall call =
      make_call("VerifySignature", SyscallNo::kVerifySignature,
                {static_cast<uint32_t>(CryptographicScheme::Ed25519),
                 signature_address(index), kSignatureStride, kPublicKeyAddress,
                 kPublicKeyBytes, kMessageAddress, message_bytes});
  call.payload_bytes = message_bytes;
  return call;
}

// The ML-DSA-65 counterpart of verify_signature_call: the same message, its own
// signatures and key.
SyscallCall
verify_signature_mldsa65_call(size_t index)
{
  const uint32_t message_bytes = kVerifyMessageBytes.at(index);
  SyscallCall call =
      make_call("VerifySignatureMlDsa65", SyscallNo::kVerifySignature,
                {static_cast<uint32_t>(CryptographicScheme::MlDsa65),
                 mldsa_signature_address(index),
                 static_cast<uint32_t>(sizeof(MlDsa65Signature)),
                 kMlDsaPublicKeyAddress,
                 static_cast<uint32_t>(sizeof(MlDsa65PublicKey)),
                 kMessageAddress, message_bytes});
  call.payload_bytes = message_bytes;
  return call;
}

// force_load distinguishes a snapshot-backed read from a frame-only read.
// These match the SDK's get_values_force and get_values paths.
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

// NonnegativeIntSetAdd(key, set_value, add_value), with a set value
// present: the shape an ERC20 balance update takes after a forced read. Never
// touches the snapshot or the db -- the write is staged in the frame's item
// view and only reaches storage at commit, which is why its first-touch rows
// need no seeding and why the commit-side cost is measured separately.
SyscallCall
nnint_set_add_call()
{
  SyscallCall call = make_call("NNIntSetAdd", SyscallNo::kNonnegativeIntSetAdd,
                               {kNNIntSetAddKeyAddress, kNNIntSetValueAddress,
                                kNNIntAddValueAddress});
  call.key_address = kNNIntSetAddKeyAddress;
  return call;
}

// Repeat writes to one key so frame state is warm and only payload size varies.
// The timed work includes bounds checks, copies, and staging in the item view;
// child-frame commit is measured separately.
SyscallCall
bytestring_set_call(uint32_t bytes)
{
  SyscallCall call =
      make_call("BytestringSet", SyscallNo::kBytestringSet,
                {kBytestringSetKeyAddress, kBytestringPayloadAddress, bytes});
  call.payload_bytes = bytes;
  call.key_address = kBytestringSetKeyAddress;
  call.row_key = {KeySpace::kBytestringSet, bytes};
  return call;
}

// BytestringGet(key, {payload, bytes}, &written_len), reading the item seeded
// at that length. The guest buffer is sized exactly to the item, which is what
// a guest that called BytestringGetLen first would do; a short buffer takes the
// truncating path instead and copies fewer bytes than it was asked for.
SyscallCall
bytestring_get_call(uint32_t bytes)
{
  SyscallCall call =
      make_call("BytestringGet", SyscallNo::kBytestringGet,
                {kBytestringGetKeyAddress, kBytestringPayloadAddress, bytes,
                 kBytestringWrittenLenAddress});
  call.payload_bytes = bytes;
  call.key_address = kBytestringGetKeyAddress;
  call.row_key = {KeySpace::kBytestring, bytes};
  return call;
}

// BytestringGetLen(key, &length). Reads the same seeded items as the Get rows
// without copying them, so the pair is what says how much of a Get is the copy.
// Its own cost should not depend on the length at all, which is why it is swept
// rather than measured once.
SyscallCall
bytestring_get_len_call(uint32_t bytes)
{
  SyscallCall call = make_call("BytestringGetLen", SyscallNo::kBytestringGetLen,
                               {kBytestringGetKeyAddress, kOutputAddressA});
  call.key_address = kBytestringGetKeyAddress;
  call.row_key = {KeySpace::kBytestring, bytes};
  return call;
}

// Use a fixed keypair and valid signatures so timings cover verification,
// without an early invalid-signature rejection.
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

  // ML-DSA-65: deterministic signatures (all-zero rnd), so every run measures
  // the same bytes.
  const auto mldsa_key = MlDsa65SigningKey::from_seed(MlDsaSeed{1});
  for (size_t i = 0; i < kVerifyMessageBytes.size(); ++i) {
    const auto signature =
        mldsa_key.sign_with_rnd(std::as_bytes(std::span(message).first(
                                    kVerifyMessageBytes[i])),
                                {}, MlDsaRnd{});
    if (!signature.has_value()) {
      std::abort();
    }
    std::ranges::copy(*signature, memory.begin() + mldsa_signature_address(i));
  }
  std::ranges::copy(mldsa_key.public_key(),
                    memory.begin() + kMlDsaPublicKeyAddress);

  // The NNInt block: the warm keys, and SetAdd's two operands. The sweep rows
  // overwrite the key per call; the operands stay.
  write_item_key(memory, kNNIntGetKeyAddress, KeySpace::kWarm, 0);
  write_item_key(memory, kNNIntSetAddKeyAddress, KeySpace::kWarm, 1);
  std::memcpy(memory.data() + kNNIntSetValueAddress, &kNNIntSetValue,
              sizeof(kNNIntSetValue));
  std::memcpy(memory.data() + kNNIntAddValueAddress, &kNNIntAddValue,
              sizeof(kNNIntAddValue));
  return memory;
}

// A failed syscall took a different path from the one under measurement.
// Fail its benchmark row while allowing the remaining JSON results to finish.
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

// Share servers and snapshot cache across rows. Setup and the first block-tip
// fetch stay outside warm measurements; cold rows manage their own fresh keys.
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
  apply_row_key(memory_span, call);

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
  if (call.payload_bytes > 0) {
    state.SetBytesProcessed(calls * static_cast<int64_t>(call.payload_bytes));
  }
  // Report declared host gas beside timing; omit ns/gas when the charge is
  // zero.
  state.counters["gas"] = benchmark::Counter(static_cast<double>(gas));
}

// Sequences keep state valid across iterations: Update precedes Finalize,
// which erases its stream. Optional one-time setup seeds Copy's source without
// including that work in every timed iteration.
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
  uint32_t payload_bytes = 0;
  for (const SyscallCall &call : timed) {
    payload_bytes += call.payload_bytes;
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
  if (payload_bytes > 0) {
    state.SetBytesProcessed(iterations * static_cast<int64_t>(payload_bytes));
  }
  state.counters["gas"] = benchmark::Counter(static_cast<double>(gas));
}

// Run a loop of real syscalls through ThreadedMachine. Arguments are set once;
// only a0 is reloaded after each status return. Timings include that reload,
// loop control, and amortized initial argument setup.
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
  // Its own fixture, not the shared one, so it needs its own setup check: a
  // fixture whose tip did not pin has no frames either, and running the guest
  // against it aborts in exactly the place this error path exists to contain.
  SyscallBenchFixture fixture;
  if (!fixture.setup_error().empty()) {
    state.SkipWithError(fixture.setup_error());
    return;
  }
  // Enough iterations per run that the program's fixed setup does not show, and
  // few enough that one run stays short next to the benchmark's own timing.
  constexpr uint32_t kIterationsPerRun = 512;
  const std::vector<uint8_t> code = make_ecall_program(call, kIterationsPerRun);
  std::vector<uint8_t> initial_memory = make_guest_memory();
  apply_row_key(initial_memory, call);
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

// Measure allocating Copy across stream-table occupancies, independently of
// the fixture's configured stream limit. Each iteration creates and destroys
// a table, seeds one stream, then copies into fresh slots. Setup and teardown
// remain timed; the /0 control isolates that overhead. Compare per-copy costs
// where setup is amortized.
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

// Measure stream creation by reset Update at fresh indices. The /0 control
// contains only table construction/destruction; no seed stream is needed.
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

// Storage rows vary prior touches, frame depth, and snapshot-cache residency:
//   entry/NNInt*          warm item view in the active frame
//   frame/Push/D          D frames opened and discarded, no syscalls
//   frame/*FirstTouch/D/K K distinct keys touched in a fresh leaf at depth D
//   frame/*NoLoad/1/K     first touch without consulting the snapshot
//   frame/*Merge/K        staged writes committed into a fresh parent
//   frame/*Discard/K      same writes discarded; control for merge cost
//   cold/NNIntGet         fresh keys loaded through the database proxy
// Frame sweeps include setup/teardown. Read per-key cost at each K; cache
// effects can make one fitted line misleading. Snapshot-cache residency depends
// on validator scheduling, so billing cannot use cache hits as a deterministic
// term.

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

// Time frame setup, one call per distinct key, and frame teardown. Report per
// iteration and count keys in items_processed. Check every call so failures
// cannot turn a row into an error-path timing.
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
  // Seed only rows that load. The large fixture working set competes for
  // cache even when the measured path never accesses it.
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
      write_item_key(memory_span, call.key_address, space, index);
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

// Every call misses the frame and snapshot cache, loads from real hog_db over
// a local socket, and inserts the result into the cache. This excludes any
// remote database hop.
//
// Consume keys once across warmup and repetitions; exhaustion fails the row.
// Use a separate discarded frame so item views do not affect subsequent rows.
// Register this row last because its snapshot-cache entries remain shared.
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
    write_item_key(memory_span, call.key_address, KeySpace::kCold, *index);
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

// Lifecycle costs live beside the syscall rows so they use the same production
// runtime, build provenance, repetitions, and gas-unit calibration. Translation
// is intentionally outside the cached invocation rows.
enum class LifecyclePhase { kSetup, kTeardown, kRoundtrip };
enum class LifecycleExit { kReturn, kTrap, kOutOfGas };

void
append_lifecycle_call(std::vector<uint32_t> &words,
                      const std::array<uint32_t, 8> &args)
{
  for (size_t i = 0; i < args.size(); ++i) {
    const auto reg = static_cast<uint8_t>(10 + i);
    const int32_t lo = static_cast<int32_t>(args[i] << 20) >> 20;
    words.push_back(rv32::test_program::u_type(
        0x37U, reg, args[i] - static_cast<uint32_t>(lo)));
    words.push_back(rv32::test_program::addi(reg, reg, lo));
  }
  words.push_back(0x00000073U);
}

Contract
lifecycle_contract(uint32_t memory_bytes, uint32_t image_bytes,
                    uint32_t code_words, uint32_t payload_bytes = 0,
                    LifecycleExit exit = LifecycleExit::kReturn)
{
  std::vector<uint32_t> words;
  // Keep the copy destinations away from each other, even for empty buffers.
  if (payload_bytes != 0) {
    append_lifecycle_call(
        words, {std::to_underlying(SyscallNo::kGetCalldata), 64,
                payload_bytes, 0, 0, 0, 4, 0});
  }
  if (exit == LifecycleExit::kTrap) {
    words.push_back(0x00100073U); // ebreak: valid code, deterministic trap
  }
  else {
    append_lifecycle_call(words,
                          {std::to_underlying(SyscallNo::kReturn), 64,
                           payload_bytes, 0, 0, 0, 0, 0});
  }
  // Unreachable but valid code: translation scales independently of execution.
  words.resize(std::max<size_t>(code_words, words.size()),
               rv32::test_program::addi(5, 5, 1));
  std::vector<uint8_t> initial(image_bytes, 0x5a);
  auto artifact = rv32::test_artifact::build_artifact(words, initial);
  // memory_bytes is the v5 header word at byte 48. This is also how the
  // artifact tests exercise an image with a zero-extended memory tail.
  rv32::store_u32_le(artifact, 48, memory_bytes);
  Contract contract(ContractType::kRv32);
  contract.rv32_contract().data.append_range(artifact);
  return contract;
}

void
bm_lifecycle_prepare(benchmark::State &state, uint32_t memory_bytes,
                      uint32_t image_bytes, uint32_t code_words)
{
  const auto contract =
      lifecycle_contract(memory_bytes, image_bytes, code_words);
  const NetworkConfigParams params(stock_network_config().v0());
  Rv32Context context;
  for (auto _ : state) {
    PreparedContract prepared;
    const auto result = context.verify_contract(contract, params, prepared);
    benchmark::DoNotOptimize(prepared.rv32);
    if (result != InvokeErrorCode::None || !prepared.rv32.has_value()) {
      state.SkipWithError("lifecycle verification failed");
      break;
    }
    // Destruction is included: the prepared image is normally retained in a
    // block cache, but its eventual release must also be accounted for.
  }
  state.counters["memory_bytes"] = memory_bytes;
  state.counters["image_bytes"] = image_bytes;
  state.counters["code_bytes"] = code_words * rv32::kInstructionBytes;
}

// Bound the batch by the recycling pool: larger batches would turn the warm
// setup row into allocation misses and teardown into overflow/free work.
constexpr size_t kLifecycleBatch = rv32::kMemorySlotPoolCap;
using LifecycleMachines =
    std::array<std::optional<rv32::ThreadedMachine>, kLifecycleBatch>;

// Manual time excludes fixture work without calling Google Benchmark's costly
// PauseTiming/ResumeTiming around each tiny operation. Report seconds per
// machine, not per batch. Do not subtract the clock floor: unresolved timings
// are marked by the report instead of manufacturing a small/negative cost.
template <typename Fn>
void
time_lifecycle_batch(benchmark::State &state, Fn &&fn)
{
  benchmark::ClobberMemory();
  const auto begin = std::chrono::steady_clock::now();
  fn();
  benchmark::ClobberMemory();
  const auto end = std::chrono::steady_clock::now();
  state.SetIterationTime(std::chrono::duration<double>(end - begin).count() /
                         static_cast<double>(kLifecycleBatch));
}

void
bm_lifecycle_timer_floor(benchmark::State &state)
{
  LifecycleMachines machines;
  for (auto _ : state) {
    time_lifecycle_batch(state, [&] {
      for (auto &machine : machines) {
        benchmark::DoNotOptimize(&machine);
        benchmark::ClobberMemory();
      }
    });
  }
  state.counters["batch_size"] = kLifecycleBatch;
}

void
bm_lifecycle_machine(benchmark::State &state, uint32_t memory_bytes,
                      LifecyclePhase phase, bool pool_miss,
                      bool batched_roundtrip)
{
  const auto contract = lifecycle_contract(memory_bytes, 0, 64);
  auto prepared = Rv32Contract::create(contract.rv32_contract(),
                                       rv32::Rv32Limits::defaults());
  if (!prepared.has_value()) {
    state.SkipWithError("lifecycle preparation failed");
    return;
  }
  LifecycleMachines machines;
  auto setup = [&] {
    for (auto &machine : machines) {
      machine.emplace(prepared->instantiate(kPlentyOfGas));
      machine->seed_method_id(1);
      benchmark::DoNotOptimize(machine->memory().data());
      benchmark::ClobberMemory();
    }
  };
  auto teardown = [&] {
    for (auto &machine : machines) {
      machine.reset();
      benchmark::ClobberMemory();
    }
  };
  // Warm all batch slots, not just one. These are the production constructors
  // and destructors; no synthetic memcpy loop or mocked recycling pool.
  setup();
  for (auto &machine : machines) {
    if (machine->memory().size() != memory_bytes ||
        !std::ranges::all_of(machine->memory(), [](uint8_t b) { return b == 0; })) {
      state.SkipWithError("lifecycle memory initialization failed");
      return;
    }
  }
  teardown();
  for (auto _ : state) {
    if (phase == LifecyclePhase::kRoundtrip && !batched_roundtrip) {
      // Keep the original uninterrupted one-machine row as an independent
      // check. It uses Google Benchmark's ordinary wall-clock timing.
      auto machine = prepared->instantiate(kPlentyOfGas);
      machine.seed_method_id(1);
      benchmark::DoNotOptimize(machine.memory().data());
      benchmark::ClobberMemory();
      continue;
    }
    if (pool_miss) {
      // Outside manual time. This means allocator misses, not cold CPU caches.
      rv32::memory_slot_pool() = {};
    }
    if (phase == LifecyclePhase::kTeardown) {
      setup();
      time_lifecycle_batch(state, teardown);
    }
    else if (phase == LifecyclePhase::kSetup) {
      time_lifecycle_batch(state, setup);
      teardown();
    }
    else {
      time_lifecycle_batch(state, [&] {
        setup();
        teardown();
      });
    }
  }
  state.counters["memory_bytes"] = memory_bytes;
  state.counters["batch_size"] =
      phase == LifecyclePhase::kRoundtrip && !batched_roundtrip ? 1
                                                              : kLifecycleBatch;
}

void
bm_lifecycle_invoke(benchmark::State &state, uint32_t memory_bytes,
                     uint32_t payload_bytes, LifecycleExit exit)
{
  auto *fixture = fixture_or_fail(state);
  if (fixture == nullptr) {
    return;
  }
  const auto contract =
      lifecycle_contract(memory_bytes, 0, 64, payload_bytes, exit);
  auto prepared = Rv32Contract::create(contract.rv32_contract(),
                                       rv32::Rv32Limits::defaults());
  if (!prepared.has_value()) {
    state.SkipWithError("lifecycle preparation failed");
    return;
  }
  Calldata calldata;
  calldata.resize(payload_bytes);
  for (size_t i = 0; i < calldata.size(); ++i) {
    calldata[i] = static_cast<uint8_t>(i * 37U + 1U);
  }
  const uint64_t gas_limit = exit == LifecycleExit::kOutOfGas ? 0 : kPlentyOfGas;
  auto invoke = [&]() {
    auto trace = fixture->handler().start_frame(
        Invocation{.invoked_address = SyscallBenchFixture::leaf_address(),
                   .entrypoint = ContractEntrypointID{},
                   .gas_limit = gas_limit,
                   .calldata = calldata},
        std::to_underlying(InvokeFlag::None), ValidatedAuthTokens{});
    if (!trace.has_value()) {
      return false;
    }
    // Heap allocation mirrors make_new_runtime's cached-contract path. No
    // mock host, reset/reuse of a live VM, or hand-written ecall loop.
    auto runtime = std::make_unique<Rv32Runtime>(&*prepared, &fixture->handler());
    const auto result = runtime->invoke(0, gas_limit)();
    runtime.reset();
    const auto finished = fixture->handler().finish_frame(
        result.result_, std::move(*trace))();
    if (!finished.has_value()) {
      return false;
    }
    if (exit == LifecycleExit::kReturn) {
      return result.is_frame_success();
    }
    const auto expected = exit == LifecycleExit::kTrap
                              ? InvokeErrorCode::Deterministic
                              : InvokeErrorCode::OutOfGas;
    return !result.is_frame_success() &&
           result.result_.error().code() == expected &&
           (exit != LifecycleExit::kOutOfGas || result.gas_consumed_ == gas_limit);
  };
  // Check the actual copied bytes outside timing, not merely a successful
  // Return: a broken GetCalldata call must not price a rejection as a copy.
  if (!invoke()) {
    state.SkipWithError("lifecycle invocation preflight failed");
    return;
  }
  if (exit == LifecycleExit::kReturn) {
    const auto returned = fixture->handler().get_return_buffer();
    if (!returned.has_value() || !returned->has_value() ||
        **returned == nullptr ||
        !std::ranges::equal(***returned, calldata)) {
      state.SkipWithError("lifecycle return payload mismatch");
      return;
    }
  }
  for (auto _ : state) {
    if (!invoke()) {
      state.SkipWithError("lifecycle invocation changed outcome");
      break;
    }
    benchmark::ClobberMemory();
  }
  state.counters["memory_bytes"] = memory_bytes;
  state.counters["payload_bytes"] = payload_bytes;
}

void
register_lifecycle_benchmarks()
{
  // Fixed samples bound untimed setup work: adaptive calibration against a
  // few-ns teardown would otherwise spend minutes constructing machines.
  benchmark::RegisterBenchmark("lifecycle/TimerFloor",
                               bm_lifecycle_timer_floor)->UseManualTime()->Iterations(100'000);
  for (uint32_t bytes : {4096U, 16384U, rv32::kDefaultMaxMemoryBytes}) {
    for (auto [name, phase] : {
             std::pair{"Setup", LifecyclePhase::kSetup},
             std::pair{"Teardown", LifecyclePhase::kTeardown}}) {
      benchmark::RegisterBenchmark(std::format("lifecycle/{}/{}", name, bytes),
                                   bm_lifecycle_machine, bytes, phase, false,
                                   false)->UseManualTime()->Iterations(100'000);
    }
    benchmark::RegisterBenchmark(
        std::format("lifecycle/SetupTeardown/{}", bytes),
        bm_lifecycle_machine, bytes, LifecyclePhase::kRoundtrip, false, false);
    benchmark::RegisterBenchmark(
        std::format("lifecycle/SetupTeardownBatch/{}", bytes),
        bm_lifecycle_machine, bytes, LifecyclePhase::kRoundtrip, false, true)
        ->UseManualTime()->Iterations(100'000);
    benchmark::RegisterBenchmark(
        std::format("lifecycle/SetupPoolMiss/{}", bytes),
        bm_lifecycle_machine, bytes, LifecyclePhase::kSetup, true, false)
        ->UseManualTime()->Iterations(100'000);
    benchmark::RegisterBenchmark(
        std::format("lifecycle/InvokeReturn/{}/0", bytes),
                                 bm_lifecycle_invoke, bytes, 0,
                                 LifecycleExit::kReturn);
    benchmark::RegisterBenchmark(std::format("lifecycle/InvokeTrap/{}", bytes),
                                 bm_lifecycle_invoke, bytes, 0,
                                 LifecycleExit::kTrap);
    benchmark::RegisterBenchmark(
        std::format("lifecycle/InvokeOutOfGas/{}", bytes),
                                 bm_lifecycle_invoke, bytes, 0,
                                 LifecycleExit::kOutOfGas);
    benchmark::RegisterBenchmark(
        std::format("lifecycle/PrepareMemory/{}", bytes),
                                 bm_lifecycle_prepare, bytes, 0, 64);
    benchmark::RegisterBenchmark(
        std::format("lifecycle/PrepareImage/{}", bytes),
                                 bm_lifecycle_prepare,
                                 rv32::kDefaultMaxMemoryBytes, bytes, 64);
  }
  for (uint32_t words : {64U, 1024U, 4096U, 16384U}) {
    benchmark::RegisterBenchmark(
        std::format("lifecycle/PrepareCode/{}", words * 4),
                                 bm_lifecycle_prepare, 4096, 0, words);
  }
  for (uint32_t bytes : {64U, 256U, static_cast<uint32_t>(kMaxReturnBufferLen)}) {
    benchmark::RegisterBenchmark(
        std::format("lifecycle/InvokeReturn/65536/{}", bytes),
                                 bm_lifecycle_invoke, 65536, bytes,
                                 LifecycleExit::kReturn);
  }
}

void
register_benchmarks()
{
  register_lifecycle_benchmarks();
  for (const SyscallCall &call : stateless_calls()) {
    benchmark::RegisterBenchmark(("entry/" + call.name).c_str(),
                                 bm_syscall_entry, call);
    benchmark::RegisterBenchmark(("vm/" + call.name).c_str(), bm_syscall_vm,
                                 call);
  }
  // Zero-byte SHA256 still compresses a padding block. Include it to check
  // the fixed crossing plus minimum hash work against larger inputs.
  for (const uint32_t bytes : {0U, 32U, 256U, 1024U, 4096U, 16384U, 32768U}) {
    const SyscallCall call = sha256_call(bytes);
    benchmark::RegisterBenchmark(
        ("entry/SHA256/" + std::to_string(bytes)).c_str(), bm_syscall_entry,
        call);
  }
  benchmark::RegisterBenchmark("vm/SHA256/0", bm_syscall_vm, sha256_call(0));
  benchmark::RegisterBenchmark("vm/SHA256/1024", bm_syscall_vm,
                               sha256_call(1024));

  // Compare incremental and one-shot hashing at the same input sizes.
  for (const uint32_t bytes : {0U, 32U, 256U, 1024U, 4096U, 16384U, 32768U}) {
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
  for (size_t i = 0; i < kVerifyMessageBytes.size(); ++i) {
    benchmark::RegisterBenchmark(("entry/VerifySignatureMlDsa65/" +
                                  std::to_string(kVerifyMessageBytes[i]))
                                     .c_str(),
                                 bm_syscall_entry,
                                 verify_signature_mldsa65_call(i));
  }
  benchmark::RegisterBenchmark(("vm/VerifySignatureMlDsa65/" +
                                std::to_string(
                                    kVerifyMessageBytes[kVerifyVmRow]))
                                   .c_str(),
                               bm_syscall_vm,
                               verify_signature_mldsa65_call(kVerifyVmRow));

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

  // Warm length sweeps expose copy cost. GetLen reads the same items without
  // copying, providing a control for length dependence.
  for (const uint32_t bytes : kBytestringBytes) {
    benchmark::RegisterBenchmark(std::format("entry/BytestringSet/{}", bytes),
                                 bm_syscall_entry, bytestring_set_call(bytes));
    benchmark::RegisterBenchmark(std::format("entry/BytestringGet/{}", bytes),
                                 bm_syscall_entry, bytestring_get_call(bytes));
    benchmark::RegisterBenchmark(std::format("entry/BytestringGetLen/{}",
                                             bytes),
                                 bm_syscall_entry,
                                 bytestring_get_len_call(bytes));
  }
  // One length through the VM for each, named with that length: the report
  // pairs vm rows to entry rows by name, and an unsuffixed name matches no
  // entry row and is dropped from the table.
  const uint32_t vm_bytes = kBytestringBytes[kBytestringVmRow];
  benchmark::RegisterBenchmark(std::format("vm/BytestringSet/{}", vm_bytes),
                               bm_syscall_vm, bytestring_set_call(vm_bytes));
  benchmark::RegisterBenchmark(std::format("vm/BytestringGet/{}", vm_bytes),
                               bm_syscall_vm, bytestring_get_call(vm_bytes));
  benchmark::RegisterBenchmark(std::format("vm/BytestringGetLen/{}", vm_bytes),
                               bm_syscall_vm,
                               bytestring_get_len_call(vm_bytes));

  // Depth counts frames above the fixture's two. The floor's depth is always
  // swept: it is the row the committed reference guards, and a chain raising
  // its limit past the floor adds the deeper row rather than moving this one.
  // Then the fixture's own maximum when it is deeper, to measure the full
  // parent walk such a chain admits (see bench_network_config).
  std::vector<uint32_t> depths = {1, 8, 16, kBenchMinCrossInvokeDepth - 2};
  const uint32_t fixture_max_depth = bench_max_cross_invoke_depth() - 2;
  if (std::ranges::find(depths, fixture_max_depth) == depths.end()) {
    depths.push_back(fixture_max_depth);
  }
  // Sweep the short key range at every depth, then the long range at depth 1.
  // Register increasing K so smaller rows do not inherit a larger cache.
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
  // Run unseeded rows first so the large database/cache working set does not
  // affect their frame-map measurements.
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
  // Measure merge/discard before seeding the large read sweep, whose
  // resident items would change the cache environment of both timings.
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

  // Run all depths over the short key range before the long depth-1 sweep.
  // Otherwise later depths inherit the large shared snapshot cache and its
  // lookup cost is misattributed to parent walking.
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

// Record the same host/build/engine provenance as the opcode calibration.
// The reporter requires a matching host and engine when converting ns to gas.
void
add_provenance_context()
{
  const rv32::gas_calibration::Provenance provenance =
      rv32::gas_calibration::collect_provenance();
  // What answered the cold row's misses. Absent in a run recorded before the
  // bench moved off the light server, which is what lets the report decline to
  // label those rather than claim this backend for them.
  benchmark::AddCustomContext("syscall_bench_item_backend", "hog_db");
  benchmark::AddCustomContext("lifecycle_timing_version", "2");
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

// Require Release timings; --allow-nonproduction is for harness development.
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

