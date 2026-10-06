// SetStorageFlags, GetStorageFlags and ClearStorageFlags.
//
// A transaction keeps one flags value per storage key. Writes never change it:
// SetStorageFlags ORs flags in, ClearStorageFlags removes them, and every push
// of the key carries whatever the value is at that point. A contract cannot
// re-enter itself, so these tests reach a key from several frames the way a
// token does: a driver invokes the token contract more than once in one
// transaction, and each token frame inherits what the earlier ones left.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

constexpr StorageDeltaFlagSet kNoFlags =
    std::to_underlying(StorageDeltaFlags::None);
constexpr StorageDeltaFlagSet kExclusive =
    std::to_underlying(StorageDeltaFlags::ExclusiveLock);

constexpr ContractAddress kDriver =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kToken =
    NativeContractTestFramework::make_contract_address(1);

constexpr std::string_view kKey = "k";
constexpr std::string_view kOtherKey = "c";

// Token methods. Each takes one calldata byte: the value to write, the entry
// to insert or expect, the flags to expect, or the error code to expect.
enum TokenMethod : Runtime::MethodID {
  kWrite = 0,             // BytestringSet(k, [arg])
  kWriteLocked = 1,       // BytestringSet(k, [arg]), SetStorageFlags(k, Excl)
  kLockExpecting = 2,     // SetStorageFlags(k, Excl) must return arg
  kExpectFlags = 3,       // GetStorageFlags(k) must return arg
  kClear = 4,             // ClearStorageFlags(k)
  kWriteLockedRevert = 5, // kWriteLocked, then revert
  kClearRevert = 6,       // ClearStorageFlags(k), then revert
  kDelete = 7,            // DeleteObject(k)
  kFailedWrite = 8,       // NNIntSetAdd(k, 0, -1), which must fail
  kSetFlagsExpecting = 9, // SetStorageFlags(k, arg) must return InvalidInput
  kAddNoFlags = 10,       // SetStorageFlags(k, None) must succeed
  kInsert = 11,           // RetrocausalQueueInsert(k, [arg])
  kConsumeExpecting = 12, // RetrocausalQueueConsume(k) must return [arg]
  kWriteOther = 13,       // BytestringSet(c, [arg])
  kClearWriteOther = 14,  // ClearStorageFlags(k), BytestringSet(c, [arg])
  kWriteObserved = 15,    // kWrite, then observe_bytes()
  kFlagsObserved = 16,    // the FlagsSyscall [arg] on k, then observe_bytes()
};

// The storage flags syscall kFlagsObserved makes.
enum class FlagsSyscall : uint8_t { kGet, kSet, kClear };

// Where the active frame's bytes for k live, as each observe_bytes() found
// them. A copy of the bytes shows up as a new address.
std::vector<uint8_t const *> observed_bytes;

uint8_t
calldata_arg(NativeSyscallContext &ctx)
{
  auto const &calldata = ctx.handler().get_calldata();
  if (calldata.size() != 1) {
    ADD_FAILURE() << "token methods take exactly one calldata byte";
    return 0;
  }
  return calldata[0];
}

x23::Async<bool>
write(NativeSyscallContext &ctx, uint8_t value,
      std::string_view key_name = kKey)
{
  auto key = NativeContractTestFramework::make_storage_key(key_name);
  auto res = co_await ctx.syscall<SyscallNo::kBytestringSet>(
      key, std::as_bytes(std::span(&value, 1)));
  if (!res.has_value() ||
      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
    ADD_FAILURE() << "BytestringSet failed";
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
insert(NativeSyscallContext &ctx, uint8_t entry)
{
  auto key = NativeContractTestFramework::make_storage_key(kKey);
  auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
      key, std::as_bytes(std::span(&entry, 1)));
  if (!res.has_value() ||
      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
    ADD_FAILURE() << "RetrocausalQueueInsert failed";
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
consume_expecting(NativeSyscallContext &ctx, uint8_t expected)
{
  auto key = NativeContractTestFramework::make_storage_key(kKey);
  uint8_t entry = 0;
  RuntimeMemoryLength written_len = 0;
  auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
      key, false, std::as_writable_bytes(std::span(&entry, 1)), written_len);
  if (!res.has_value() ||
      res.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
      written_len != 1 || entry != expected) {
    ADD_FAILURE() << "RetrocausalQueueConsume returned "
                  << static_cast<int>(entry) << ", expected "
                  << static_cast<int>(expected);
    co_return false;
  }
  co_return true;
}

x23::Async<HostFnStatus<SyscallErrorCode>>
set_flags(NativeSyscallContext &ctx, StorageDeltaFlagSet flags)
{
  auto key = NativeContractTestFramework::make_storage_key(kKey);
  co_return co_await ctx.syscall<SyscallNo::kSetStorageFlags>(key, flags);
}

x23::Async<bool>
lock(NativeSyscallContext &ctx)
{
  auto res = co_await set_flags(ctx, kExclusive);
  if (!res.has_value() ||
      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
    ADD_FAILURE() << "SetStorageFlags(ExclusiveLock) failed";
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
clear(NativeSyscallContext &ctx)
{
  auto key = NativeContractTestFramework::make_storage_key(kKey);
  auto res = co_await ctx.syscall<SyscallNo::kClearStorageFlags>(key);
  if (!res.has_value() ||
      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
    ADD_FAILURE() << "ClearStorageFlags failed";
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
observe_bytes(NativeSyscallContext &ctx)
{
  auto key = NativeContractTestFramework::make_storage_key(kKey);
  auto read = co_await ctx.handler().get_bytestring(key);
  if (!read.has_value() || !read.value().result.has_value()) {
    ADD_FAILURE() << "get_bytestring failed";
    co_return false;
  }
  observed_bytes.push_back(read.value().result.value().data());
  co_return true;
}

x23::Async<bool>
flags_syscall(NativeSyscallContext &ctx, FlagsSyscall syscall)
{
  switch (syscall) {
  case FlagsSyscall::kGet:
  {
    auto key = NativeContractTestFramework::make_storage_key(kKey);
    StorageDeltaFlagSet flags = kNoFlags;
    auto res = co_await ctx.syscall<SyscallNo::kGetStorageFlags>(key, flags);
    co_return res.has_value() &&
        res.value() == SyscallErrorCode::kSyscallErrorCodeNone;
  }
  case FlagsSyscall::kSet:
    co_return co_await lock(ctx);
  case FlagsSyscall::kClear:
    co_return co_await clear(ctx);
  }
  ADD_FAILURE() << "unknown FlagsSyscall";
  co_return false;
}

InvokeStatus<void>
ok_or_fail(bool ok)
{
  if (!ok) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  return {};
}

std::unique_ptr<NativeContract>
make_token()
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(kWrite,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(co_await write(ctx, calldata_arg(ctx)));
              });
  fns.emplace(kWriteLocked,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(co_await write(ctx, calldata_arg(ctx)) &&
                                     co_await lock(ctx));
              });
  fns.emplace(kLockExpecting,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto const expected =
                    static_cast<SyscallErrorCode>(calldata_arg(ctx));
                auto res = co_await set_flags(ctx, kExclusive);
                if (!res.has_value() || res.value() != expected) {
                  ADD_FAILURE()
                      << "SetStorageFlags(ExclusiveLock) returned "
                      << (res.has_value() ? static_cast<int>(res.value()) : -1)
                      << ", expected " << static_cast<int>(expected);
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  fns.emplace(kExpectFlags,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto const expected =
                    static_cast<StorageDeltaFlagSet>(calldata_arg(ctx));
                auto key = NativeContractTestFramework::make_storage_key(kKey);
                StorageDeltaFlagSet flags = -1;
                auto res =
                    co_await ctx.syscall<SyscallNo::kGetStorageFlags>(key,
                                                                      flags);
                if (!res.has_value() ||
                    res.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
                    flags != expected) {
                  ADD_FAILURE() << "GetStorageFlags returned " << flags
                                << ", expected " << expected;
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  fns.emplace(kClear,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(co_await clear(ctx));
              });
  fns.emplace(kWriteLockedRevert,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                if (co_await write(ctx, calldata_arg(ctx))) {
                  co_await lock(ctx);
                }
                co_return InvokeError{InvokeErrorCode::Deterministic};
              });
  fns.emplace(kClearRevert,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_await clear(ctx);
                co_return InvokeError{InvokeErrorCode::Deterministic};
              });
  fns.emplace(kDelete,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto key = NativeContractTestFramework::make_storage_key(kKey);
                auto res = co_await ctx.syscall<SyscallNo::kDeleteObject>(key);
                co_return ok_or_fail(
                    res.has_value() &&
                    res.value() == SyscallErrorCode::kSyscallErrorCodeNone);
              });
  fns.emplace(
      kFailedWrite,
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = NativeContractTestFramework::make_storage_key(kKey);
        int128_t set_value = 0;
        auto res =
            co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(key,
                                                                   &set_value,
                                                                   -1);
        if (!res.has_value() ||
            res.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          ADD_FAILURE() << "NNIntSetAdd(0, -1) should fail";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns.emplace(kSetFlagsExpecting,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto res =
                    co_await set_flags(ctx, static_cast<StorageDeltaFlagSet>(
                                                calldata_arg(ctx)));
                co_return ok_or_fail(
                    res.has_value() &&
                    res.value() ==
                        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
              });
  fns.emplace(kAddNoFlags,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto res = co_await set_flags(ctx, kNoFlags);
                co_return ok_or_fail(
                    res.has_value() &&
                    res.value() == SyscallErrorCode::kSyscallErrorCodeNone);
              });
  fns.emplace(kInsert,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(co_await insert(ctx, calldata_arg(ctx)));
              });
  fns.emplace(kConsumeExpecting,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(
                    co_await consume_expecting(ctx, calldata_arg(ctx)));
              });
  fns.emplace(kWriteOther,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(
                    co_await write(ctx, calldata_arg(ctx), kOtherKey));
              });
  fns.emplace(kClearWriteOther,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(
                    co_await clear(ctx) &&
                    co_await write(ctx, calldata_arg(ctx), kOtherKey));
              });
  fns.emplace(kWriteObserved,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(co_await write(ctx, calldata_arg(ctx)) &&
                                     co_await observe_bytes(ctx));
              });
  fns.emplace(kFlagsObserved,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                co_return ok_or_fail(
                    co_await flags_syscall(ctx, static_cast<FlagsSyscall>(
                                                    calldata_arg(ctx))) &&
                    co_await observe_bytes(ctx));
              });
  return std::make_unique<NativeContract>(std::move(fns));
}

// One step of a driver script: a token method, its calldata byte, and how the
// driver calls it.
struct Step {
  TokenMethod method;
  uint8_t arg = 0;
  bool try_invoke = false;
  // Whether the token frame is expected to return, as opposed to revert (or,
  // for a TryInvoke, to fail lock acquisition).
  bool returns = true;
};

uint8_t
code(SyscallErrorCode c)
{
  return static_cast<uint8_t>(c);
}

x23::Async<bool>
run_step(NativeSyscallContext &ctx, Step const &step)
{
  const uint8_t arg = step.arg;
  CopyableInvokeInputs inputs{
      .entrypoint_id = step.method,
      .gas_limit = UINT64_MAX,
      .flags = step.try_invoke
                   ? InvokeFlagSet{std::to_underlying(InvokeFlag::TryInvoke)}
                   : InvokeFlagSet{std::to_underlying(InvokeFlag::None)},
  };
  CopyableInvokeOutputs outputs{};
  auto res = co_await ctx.syscall<SyscallNo::kInvoke>(
      kToken, std::as_bytes(std::span(&arg, 1)), std::span<uint32_t>{}, inputs,
      outputs);
  if (!res.has_value() ||
      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
    ADD_FAILURE() << "Invoke of token method " << step.method << " failed";
    co_return false;
  }
  const bool returned =
      outputs.frame_status == InvocationFrameStatusCode::Return;
  if (returned != step.returns) {
    ADD_FAILURE() << "token method " << step.method
                  << (returned ? " returned" : " did not return");
    co_return false;
  }
  co_return true;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class StorageFlagsTest : public ::testing::Test,
                         public NativeContractTestFramework {
public:
  StorageFlagsTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override
  {
    // The driver's method 0 runs whatever script the test last installed.
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> driver;
    driver.emplace(
        0, [this](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          for (auto const &step : script_) {
            if (!co_await run_step(ctx, step)) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }
          }
          co_return InvokeStatus<void>{};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contracts;
    contracts.emplace(0, std::make_unique<NativeContract>(std::move(driver)));
    contracts.emplace(1, make_token());
    context_ = std::make_unique<NativeContext>(std::move(contracts));

    ContractAddress const addrs[] = {kDriver, kToken};
    for (int32_t i = 0; i < 2; ++i) {
      auto contract = make_native_contract(i);
      write_contract(contract);
      write_deployment(addrs[i], contract);
      setup_grant_storage_token(addrs[i], 1'000'000'000);
    }
  }

  Transaction make_tx(ContractAddress const &address, Runtime::MethodID method,
                      Calldata calldata = {})
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = address,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = std::move(calldata)},
        .uniquifier = make_new_uniquifier(),
    };
  }

  // Runs [script] as one transaction and commits it. Returns the flags the
  // root frame pushes for the token's key, or nullopt if it pushes no delta.
  std::optional<StorageDeltaFlagSet> run_script(std::vector<Step> script)
  {
    script_ = std::move(script);
    auto handler = make_new_system_call_handler(make_tx(kDriver, 0), *context_);
    handler.enable_tracing();
    auto res = handler.root_invoke()();
    EXPECT_TRUE(res.is_root_success());
    EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto const *trace = handler.get_trace();
    EXPECT_NE(trace, nullptr);
    if (trace == nullptr) {
      return std::nullopt;
    }
    auto const addrkey = make_contract_data_addrkey(kToken, kKey);
    for (auto const &write : trace->storage_writes) {
      if (write.addrkey == addrkey) {
        return write.flags;
      }
    }
    return std::nullopt;
  }

  // Runs one token method as a transaction of its own, in the current block,
  // and returns the commit result.
  ExecResult token_transaction(TokenMethod method, uint8_t arg)
  {
    auto handler =
        make_new_system_call_handler(make_tx(kToken, method, Calldata{arg}),
                                     *context_);
    auto res = handler.root_invoke()();
    EXPECT_TRUE(res.is_root_success());
    return handler.try_commit_transaction()();
  }

  // A second transaction in the same block writes the key directly. HogDb
  // turns away a same-block bytestring write of a different value on its own
  // (ValueMismatch), so a test that probes a lock writes the value the key
  // already holds: then only the lock can refuse it.
  ExecResult write_from_another_transaction(uint8_t value)
  {
    return token_transaction(kWrite, value);
  }

  std::optional<uint8_t> stored_value(std::string_view key_name = kKey)
  {
    auto item = lookup_storage_item_from_db(
        make_contract_data_addrkey(kToken, key_name));
    if (!item.has_value() ||
        item->type() != StorageItemType::kItemTypeBytestring ||
        item->bytestring().size() != 1) {
      return std::nullopt;
    }
    return item->bytestring()[0];
  }

  // The number of live entries in the queue at k, or nullopt if k does not
  // hold a queue.
  std::optional<uint64_t> queue_length()
  {
    auto item =
        lookup_storage_item_from_db(make_contract_data_addrkey(kToken, kKey));
    if (!item.has_value() ||
        item->type() != StorageItemType::kItemTypeRetrocausalQueue) {
      return std::nullopt;
    }
    return item->metadata().live_entries;
  }

  std::unique_ptr<RuntimeContext> context_;
  std::vector<Step> script_;
};

TEST_F(StorageFlagsTest, FlagsNeedAPendingWrite)
{
  const uint8_t invalid =
      code(SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
  const uint8_t none = code(SyscallErrorCode::kSyscallErrorCodeNone);

  // An unwritten key has no flags and takes none.
  EXPECT_EQ(run_script({
                {.method = kExpectFlags, .arg = kNoFlags},
                {.method = kLockExpecting, .arg = invalid},
                {.method = kClear},
                {.method = kWrite, .arg = 1},
            }),
            kNoFlags);
  advance_block();
  ASSERT_EQ(stored_value(), 1);

  // A value that exists only in the snapshot is not a pending write either.
  // Deleting it is, and flags attach to the deletion.
  EXPECT_EQ(run_script({
                {.method = kLockExpecting, .arg = invalid},
                {.method = kDelete},
                {.method = kLockExpecting, .arg = none},
                {.method = kExpectFlags, .arg = kExclusive},
            }),
            kExclusive);
}

TEST_F(StorageFlagsTest, SetStorageFlagsRejectsUnknownFlags)
{
  EXPECT_EQ(run_script({
                {.method = kWrite, .arg = 1},
                {.method = kSetFlagsExpecting, .arg = 2},
                {.method = kExpectFlags, .arg = kNoFlags},
            }),
            kNoFlags);
}

TEST_F(StorageFlagsTest, LaterWritesAndFramesKeepFlags)
{
  // Each step is its own token frame: the lock set by the first survives a
  // frame that only reads, a plain write, a deletion, and a failed write, and
  // the root pushes it with the final value.
  EXPECT_EQ(run_script({
                {.method = kWriteLocked, .arg = 1},
                {.method = kExpectFlags, .arg = kExclusive},
                {.method = kWrite, .arg = 2},
                {.method = kDelete},
                {.method = kFailedWrite},
                {.method = kWrite, .arg = 3},
                {.method = kExpectFlags, .arg = kExclusive},
            }),
            kExclusive);
}

TEST_F(StorageFlagsTest, SetStorageFlagsAdds)
{
  // Adding None, or ExclusiveLock again, removes nothing, and neither does a
  // rejected set.
  EXPECT_EQ(run_script({
                {.method = kWriteLocked, .arg = 1},
                {.method = kAddNoFlags},
                {.method = kLockExpecting,
                 .arg = code(SyscallErrorCode::kSyscallErrorCodeNone)},
                {.method = kSetFlagsExpecting, .arg = 2},
                {.method = kExpectFlags, .arg = kExclusive},
            }),
            kExclusive);
}

TEST_F(StorageFlagsTest, RevertedFramesDiscardFlagChanges)
{
  // A lock set in a reverted frame, or in a TryInvoke that reverts, goes with
  // that frame's write; so does a clear.
  EXPECT_EQ(run_script({
                {.method = kWrite, .arg = 1},
                {.method = kWriteLockedRevert, .arg = 2, .returns = false},
                {.method = kExpectFlags, .arg = kNoFlags},
                {.method = kWriteLockedRevert,
                 .arg = 2,
                 .try_invoke = true,
                 .returns = false},
                {.method = kExpectFlags, .arg = kNoFlags},
                {.method = kWriteLocked, .arg = 3},
                {.method = kClearRevert, .returns = false},
                {.method = kClearRevert, .try_invoke = true, .returns = false},
                {.method = kExpectFlags, .arg = kExclusive},
            }),
            kExclusive);
  advance_block();
  EXPECT_EQ(stored_value(), 3);
}

// A TryInvoke frame that returns must still acquire its locks, and when HogDb
// refuses them the frame fails as a reverted one does, flag changes included.
// Here the refusal is the lock itself: another transaction wrote k earlier in
// the block, so an exclusive lock on k is denied, while the same write without
// it is admitted (the root commit shows it).
TEST_F(StorageFlagsTest, RefusedTryInvokeDiscardsItsLock)
{
  ASSERT_EQ(write_from_another_transaction(1), ExecResult::ExecResultOK);

  EXPECT_EQ(run_script({
                {.method = kWrite, .arg = 1},
                {.method = kWriteLocked,
                 .arg = 1,
                 .try_invoke = true,
                 .returns = false},
                {.method = kExpectFlags, .arg = kNoFlags},
            }),
            kNoFlags);

  // Nor did the lock reach HogDb.
  EXPECT_EQ(write_from_another_transaction(1), ExecResult::ExecResultOK);
  advance_block();
  EXPECT_EQ(stored_value(), 1);
}

// The same for a clear, refused over another key: the TryInvoke clears k's
// lock and writes c, which another transaction wrote with a different value
// earlier in the block (ValueMismatch). The whole push is refused, so k keeps
// its lock and c its value.
TEST_F(StorageFlagsTest, RefusedTryInvokeDiscardsItsClear)
{
  ASSERT_EQ(token_transaction(kWriteOther, 5), ExecResult::ExecResultOK);

  EXPECT_EQ(run_script({
                {.method = kWriteLocked, .arg = 1},
                {.method = kClearWriteOther,
                 .arg = 6,
                 .try_invoke = true,
                 .returns = false},
                {.method = kExpectFlags, .arg = kExclusive},
            }),
            kExclusive);

  EXPECT_EQ(write_from_another_transaction(1),
            ExecResult::ExecResultTxNonDeterministicError);
  advance_block();
  EXPECT_EQ(stored_value(), 1);
  EXPECT_EQ(stored_value(kOtherKey), 5);
}

TEST_F(StorageFlagsTest, CommittedLockKeepsOtherWritersOutForTheBlock)
{
  // The lock set before a later plain write still freezes the key.
  EXPECT_EQ(run_script({
                {.method = kWriteLocked, .arg = 1},
                {.method = kWrite, .arg = 2},
            }),
            kExclusive);
  EXPECT_EQ(write_from_another_transaction(2),
            ExecResult::ExecResultTxNonDeterministicError);
  advance_block();
  EXPECT_EQ(stored_value(), 2);

  // Next block, the key is open again.
  EXPECT_EQ(write_from_another_transaction(9), ExecResult::ExecResultOK);
  advance_block();
  EXPECT_EQ(stored_value(), 9);
}

TEST_F(StorageFlagsTest, UnlockedWriteLeavesOtherWritersIn)
{
  EXPECT_EQ(run_script({{.method = kWrite, .arg = 1}}), kNoFlags);
  EXPECT_EQ(write_from_another_transaction(1), ExecResult::ExecResultOK);
  advance_block();
  EXPECT_EQ(stored_value(), 1);
}

// #4303: a flags syscall in a fresh token frame copies the root's view of k
// into that frame, and must share the bytestring rather than copy it.
TEST_F(StorageFlagsTest, FlagsSyscallsDoNotCopyTheBytestring)
{
  observed_bytes.clear();
  auto flags_step = [](FlagsSyscall syscall) {
    return Step{.method = kFlagsObserved, .arg = std::to_underlying(syscall)};
  };
  EXPECT_EQ(run_script({
                {.method = kWriteObserved, .arg = 1},
                flags_step(FlagsSyscall::kGet),
                flags_step(FlagsSyscall::kSet),
                flags_step(FlagsSyscall::kClear),
            }),
            kNoFlags);

  ASSERT_EQ(observed_bytes.size(), 4U);
  for (size_t i = 1; i < observed_bytes.size(); ++i) {
    EXPECT_EQ(observed_bytes[i], observed_bytes[0]) << "token frame " << i;
  }
}

// A TryInvoke that returns pushes its frame's changes, flags included, before
// the transaction commits. Clearing afterwards withdraws the freeze at the
// commit push; without the clear, the freeze stands.
class StorageFlagsClearAfterPushTest
  : public StorageFlagsTest,
    public ::testing::WithParamInterface<bool> {};

TEST_P(StorageFlagsClearAfterPushTest, ClearWithdrawsAPushedLock)
{
  const bool clear_after_push = GetParam();
  std::vector<Step> script = {
      {.method = kWriteLocked, .arg = 1, .try_invoke = true},
  };
  if (clear_after_push) {
    script.push_back({.method = kClear});
  }
  EXPECT_EQ(run_script(std::move(script)),
            clear_after_push ? kNoFlags : kExclusive);

  EXPECT_EQ(write_from_another_transaction(1),
            clear_after_push ? ExecResult::ExecResultOK
                             : ExecResult::ExecResultTxNonDeterministicError);
  advance_block();
  EXPECT_EQ(stored_value(), 1);
}

INSTANTIATE_TEST_SUITE_P(StorageFlags, StorageFlagsClearAfterPushTest,
                         ::testing::Bool());

// An insert into a key holding a bytestring replaces it with a queue (#1673).
// The handler swaps the view's item for a new queue item, later inserts and
// consumes change that item in place, and none of it touches the key's flags:
// a lock set on the bytestring holds the queue, and the competing insert from
// another transaction is refused. k holds a committed bytestring, so HogDb sees
// the type change too. Unlocked, the competing insert joins the queue.
class StorageFlagsQueueConversionTest
  : public StorageFlagsTest,
    public ::testing::WithParamInterface<bool> {};

TEST_P(StorageFlagsQueueConversionTest, LockSurvivesConversionToAQueue)
{
  const bool locked = GetParam();
  EXPECT_EQ(run_script({{.method = kWrite, .arg = 1}}), kNoFlags);
  advance_block();
  ASSERT_EQ(stored_value(), 1);

  const StorageDeltaFlagSet flags = locked ? kExclusive : kNoFlags;
  const auto flags_arg = static_cast<uint8_t>(flags);
  EXPECT_EQ(run_script({
                {.method = locked ? kWriteLocked : kWrite, .arg = 2},
                {.method = kInsert, .arg = 5},
                {.method = kExpectFlags, .arg = flags_arg},
                {.method = kInsert, .arg = 6},
                {.method = kConsumeExpecting, .arg = 5},
                {.method = kExpectFlags, .arg = flags_arg},
            }),
            flags);

  EXPECT_EQ(token_transaction(kInsert, 7),
            locked ? ExecResult::ExecResultTxNonDeterministicError
                   : ExecResult::ExecResultOK);
  advance_block();
  // Entry 5 was consumed; 7 is there only if the competing insert committed.
  EXPECT_EQ(queue_length(), locked ? 1U : 2U);
}

INSTANTIATE_TEST_SUITE_P(StorageFlags, StorageFlagsQueueConversionTest,
                         ::testing::Bool());

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
