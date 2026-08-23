#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {

class RCQDeleteTest : public ::testing::TestWithParam<TestDB>,
                      public NativeContractTestFramework {
protected:
  RCQDeleteTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  std::vector<uint8_t> last_consumed_values_;

  inline static std::atomic<bool> wait_flag_1_ = false;
  inline static std::atomic<bool> wait_flag_2_ = false;

  constexpr static auto kStorageKey = make_storage_key("rcq");
  constexpr static ContractAddress kContractAddress =
      make_contract_address(0); // Default address

  static AddressAndKey get_queue_addrkey()
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = kStorageKey;
    return AddressAndKey{.address = kContractAddress, .key = key};
  }

  // The RCQ operations behind entrypoints 0, 1 and 2, callable directly.
  //
  // Methods that need to sequence several of these used to do it by invoking
  // those entrypoints on their own address, which the reentrance guard now
  // rejects. Calling them in-frame instead is equivalent for what these tests
  // assert: a queue is addressed by the executing frame's own address, so two
  // frames touching one queue means one address twice on the invocation stack
  // -- something the guard makes unreachable in production. Observations are
  // recorded per transaction, keyed by (address, key, index), not per frame,
  // so the observation sequence is unchanged either way; and a child frame
  // that returns normally commits into its parent, so no savepoint behaviour
  // is lost. Only the TryInvoke-based methods, where a child frame is reverted
  // on purpose, still need real frames.
  //
  // Each returns true on success; callers report and bail on false.
  [[nodiscard]] static x23::Async<bool> rcq_insert(NativeSyscallContext &ctx,
                                                   uint32_t value)
  {
    auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
        kStorageKey, std::as_bytes(std::span(&value, 1)));
    co_return res.has_value() &&
        res.value() == SyscallErrorCode::kSyscallErrorCodeNone;
  }

  [[nodiscard]] static x23::Async<bool>
  rcq_consume_expect(NativeSyscallContext &ctx, uint32_t expected)
  {
    uint32_t out = 0;
    RuntimeMemoryLength written_len = 0;
    auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
        kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
        written_len);
    co_return res.has_value() &&
        res.value() == SyscallErrorCode::kSyscallErrorCodeNone &&written_len ==
            4 &&
        out == expected;
  }

  [[nodiscard]] static x23::Async<bool>
  rcq_consume_expect_empty(NativeSyscallContext &ctx)
  {
    uint32_t out = 0;
    RuntimeMemoryLength written_len = 0;
    auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
        kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
        written_len);
    co_return res.has_value() &&
        ((res.value() ==
          SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) ||
         (res.value() ==
          SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist)) &&
        written_len == 0;
  }

  // Mirrors entrypoint 24: peek does not advance the queue.
  [[nodiscard]] static x23::Async<bool>
  rcq_peek_expect(NativeSyscallContext &ctx, uint32_t expected)
  {
    uint32_t peeked = 0;
    RuntimeMemoryLength peek_len = 0;
    auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
        kStorageKey, true, std::as_writable_bytes(std::span(&peeked, 1)),
        peek_len);
    co_return res.has_value() &&
        res.value() == SyscallErrorCode::kSyscallErrorCodeNone &&peek_len ==
            4 &&
        peeked == expected;
  }

  // Mirrors entrypoint 4: consuming a key that is not a queue must fail.
  [[nodiscard]] static x23::Async<bool>
  rcq_consume_expect_failure(NativeSyscallContext &ctx)
  {
    uint32_t out = 0;
    RuntimeMemoryLength written_len = 0;
    auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
        kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
        written_len);
    co_return res.has_value() &&
        res.value() != SyscallErrorCode::kSyscallErrorCodeNone;
  }

  [[nodiscard]] static x23::Async<bool> rcq_delete(NativeSyscallContext &ctx)
  {
    auto res = co_await ctx.syscall<SyscallNo::kDeleteObject>(
        kStorageKey, std::to_underlying(StorageDeltaFlags::None));
    co_return res.has_value() &&
        res.value() == SyscallErrorCode::kSyscallErrorCodeNone;
  }

  // NOLINTNEXTLINE(readability-function-size)
  static std::unique_ptr<RuntimeContext> make_context()
  {
    NativeContractBuilder builder;
    builder.add<SerializationFormat::kTxs>(
        0,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          // insert an entry to the queue
          auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
              kStorageKey, std::as_bytes(std::span(&value, 1)));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "failed to insert element to RCQ";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    builder.add<SerializationFormat::kTxs>(
        1,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          // consume an entry, and check that against the supplied input
          uint32_t out = 0;
          RuntimeMemoryLength written_len = 0;

          auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
              written_len);
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "failed to consume element to RCQ";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (written_len != 4 || out != value) {
            ADD_FAILURE() << "Got bad result from RCQ: expected " << value
                          << " but got " << out;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    builder.add<SerializationFormat::kTxs>(
        2, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          // consume an entry, expect to get nothing
          uint32_t out = 0;
          RuntimeMemoryLength written_len = 0;

          auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
              written_len);
          if (!res.has_value() ||
              ((res.value() !=
                SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) &&
               (res.value() !=
                SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist))) {
            ADD_FAILURE() << "Got unexpected item from rcq";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (written_len != 0) {
            ADD_FAILURE() << "Got bad result from RCQ";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    builder.add<SerializationFormat::kTxs>(
        3,
        [](NativeSyscallContext &ctx,
           Bytestring bytestring) -> x23::Async<InvokeStatus<void>> {
          // Write a bytestring
          auto res = co_await ctx.syscall<SyscallNo::kBytestringSet>(
              kStorageKey, std::as_bytes(std::span(bytestring)),
              std::to_underlying(StorageDeltaFlags::None));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to set bytestring";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    builder.add<SerializationFormat::kTxs>(
        4, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          // try to consume, expect invalid syscall input

          uint32_t out = 0;
          RuntimeMemoryLength written_len = 0;
          auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
              written_len);
          if (!res.has_value() ||
              res.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Consume should have failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    // Test for NoDoubleConsumeDeletedEntries:
    // Consume -> Delete -> Consume

    builder.add<SerializationFormat::kTxs>(
        10,
        [](NativeSyscallContext &ctx, uint32_t consume_a,
           uint32_t consume_b) -> x23::Async<InvokeStatus<void>> {
          // consume -> delete -> consume
          if (!co_await rcq_consume_expect(ctx, consume_a)) {
            ADD_FAILURE() << "Failed to consume " << consume_a;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_consume_expect(ctx, consume_b)) {
            ADD_FAILURE() << "Failed to consume " << consume_b;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Test for NoDeleteUnobservedEntries
    // consume (nothing observed) -> delete -> consume
    //
    // The two consumes used to run in child frames, via kInvoke back into this
    // same contract, which the reentrance guard now rejects. They are done
    // inline instead: queue-entry observations are recorded per transaction,
    // keyed by (address, key, index) (see ProposalModeData), not per frame, so
    // the observation sequence this test turns on is unchanged. Neither child
    // reverted or used TryInvoke, so the frame boundary carried no savepoint
    // behaviour either.
    builder.add<SerializationFormat::kTxs>(
        11,
        [](NativeSyscallContext &ctx,
           uint32_t consume_b) -> x23::Async<InvokeStatus<void>> {
          // consume, observing nothing: the queue is empty at this point
          {
            uint32_t out = 0;
            RuntimeMemoryLength written_len = 0;
            auto res =
                co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                    kStorageKey, false,
                    std::as_writable_bytes(std::span(&out, 1)), written_len);
            if (!res.has_value() ||
                ((res.value() !=
                  SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) &&
                 (res.value() !=
                  SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist)) ||
                written_len != 0) {
              ADD_FAILURE()
                  << "co_await consume(expect none) did not come back empty";
              // Wake the main thread: it blocks on wait_flag_1_, so returning
              // without setting the flag deadlocks the test instead of failing
              // it. (Same guard as the delete branch below.)
              wait_flag_1_ = true;
              wait_flag_1_.notify_all();
              co_return InvokeError{InvokeErrorCode::None};
            }
          }

          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            // stop deadlock on test failure
            wait_flag_1_ = true;
            wait_flag_1_.notify_all();
            co_return InvokeError{InvokeErrorCode::None};
          }

          wait_flag_1_ = true;
          wait_flag_1_.notify_all();

          // The main thread inserts consume_b concurrently, in its own
          // transaction, while this frame is parked here.
          wait_flag_2_.wait(false);

          // That entry was never observed by this transaction, so the delete
          // above must not have removed it.
          {
            uint32_t out = 0;
            RuntimeMemoryLength written_len = 0;
            auto res =
                co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                    kStorageKey, false,
                    std::as_writable_bytes(std::span(&out, 1)), written_len);
            if (!res.has_value() ||
                res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "Failed to consume concurrently inserted entry";
              co_return InvokeError{InvokeErrorCode::None};
            }
            if (written_len != 4 || out != consume_b) {
              ADD_FAILURE() << "Got bad result from RCQ: expected " << consume_b
                            << " but got " << out;
              co_return InvokeError{InvokeErrorCode::None};
            }
          }

          co_return InvokeStatus<void>{};
        });

    // Test for DeleteSelfInsertedEntry
    builder.add<SerializationFormat::kTxs>(
        12,
        [](NativeSyscallContext &ctx, uint32_t insert_a,
           uint32_t consume_b) -> x23::Async<InvokeStatus<void>> {
          // insert -> delete -> consume
          if (!co_await rcq_insert(ctx, insert_a)) {
            ADD_FAILURE() << "Failed to insert";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, consume_b)) {
            ADD_FAILURE() << "Failed to consume " << consume_b;
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // Test for DeleteAndRecreateRemovesPastBlocks
    builder.add<SerializationFormat::kTxs>(
        13,
        [](NativeSyscallContext &ctx,
           uint32_t insert_1) -> x23::Async<InvokeStatus<void>> {
          // delete -> insert
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_insert(ctx, insert_1)) {
            ADD_FAILURE() << "Failed to insert";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // ConsumeOneOfManyDeleteConsumeRest:
    // co_await consume(a) -> delete -> co_await consume(b) -> co_await
    // consume(c)
    builder.add<SerializationFormat::kTxs>(
        14,
        [](NativeSyscallContext &ctx, uint32_t consume_a, uint32_t consume_b,
           uint32_t consume_c) -> x23::Async<InvokeStatus<void>> {
          // co_await consume(a) -> delete -> co_await consume(b) -> co_await
          // consume(c)
          if (!co_await rcq_consume_expect(ctx, consume_a)) {
            ADD_FAILURE() << "Failed to co_await consume(a) " << consume_a;
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, consume_b)) {
            ADD_FAILURE() << "Failed to co_await consume(b) " << consume_b;
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, consume_c)) {
            ADD_FAILURE() << "Failed to co_await consume(c) " << consume_c;
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // InsertSelfConsumeSelfDeleteInsertConsume:
    // insert(a) -> co_await consume(a) -> delete -> insert(b) -> co_await
    // consume(b)
    builder.add<SerializationFormat::kTxs>(
        15,
        [](NativeSyscallContext &ctx, uint32_t insert_a,
           uint32_t insert_b) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_insert(ctx, insert_a)) {
            ADD_FAILURE() << "Failed to insert(a)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, insert_a)) {
            ADD_FAILURE() << "Failed to co_await consume(a)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_insert(ctx, insert_b)) {
            ADD_FAILURE() << "Failed to insert(b)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, insert_b)) {
            ADD_FAILURE() << "Failed to co_await consume(b)";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // DeleteConsumeEmpty: delete -> co_await consume(expect nothing)
    builder.add<SerializationFormat::kTxs>(
        17, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect_empty(ctx)) {
            ADD_FAILURE() << "consume did not come back empty";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // ConsumeEmptyDeleteConsumeEmpty:
    // co_await consume(nothing) -> delete -> co_await consume(nothing)
    builder.add<SerializationFormat::kTxs>(
        18, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_consume_expect_empty(ctx)) {
            ADD_FAILURE() << "first consume did not come back empty";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect_empty(ctx)) {
            ADD_FAILURE() << "second consume did not come back empty";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // MultipleDeleteRecreate:
    // co_await consume(a) -> delete -> co_await consume(b) -> delete ->
    // co_await consume(c)
    builder.add<SerializationFormat::kTxs>(
        19,
        [](NativeSyscallContext &ctx, uint32_t consume_a, uint32_t consume_b,
           uint32_t consume_c) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_consume_expect(ctx, consume_a)) {
            ADD_FAILURE() << "Failed to co_await consume(a) " << consume_a;
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue (first)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, consume_b)) {
            ADD_FAILURE() << "Failed to co_await consume(b) " << consume_b;
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue (second)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, consume_c)) {
            ADD_FAILURE() << "Failed to co_await consume(c) " << consume_c;
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // DeleteInsertDeleteInsertConsume:
    // delete -> insert(a) -> delete -> insert(b) -> co_await
    // consume(concurrent)
    builder.add<SerializationFormat::kTxs>(
        20,
        [](NativeSyscallContext &ctx, uint32_t insert_a, uint32_t insert_b,
           uint32_t consume_c) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue (first)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_insert(ctx, insert_a)) {
            ADD_FAILURE() << "Failed to insert(a)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue (second)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_insert(ctx, insert_b)) {
            ADD_FAILURE() << "Failed to insert(b)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, insert_b)) {
            ADD_FAILURE() << "Failed to consume self(b)";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, consume_c)) {
            ADD_FAILURE() << "Failed to co_await consume(c) " << consume_c;
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // PeekDeleteConsume:
    // peek(expected) -> delete -> co_await consume(expected)
    builder.add<SerializationFormat::kTxs>(
        21,
        [](NativeSyscallContext &ctx,
           uint32_t expected) -> x23::Async<InvokeStatus<void>> {
          // peek
          uint32_t peeked = 0;
          RuntimeMemoryLength peek_len = 0;
          auto rp = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, true, std::as_writable_bytes(std::span(&peeked, 1)),
              peek_len);
          if (!rp.has_value() ||
              rp.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to peek";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (peek_len != 4 || peeked != expected) {
            ADD_FAILURE() << "Peek: expected " << expected << " but got "
                          << peeked;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // delete
          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // consume (should get same entry since peek doesn't advance)
          if (!co_await rcq_consume_expect(ctx, expected)) {
            ADD_FAILURE() << "Failed to consume " << expected
                          << " after peek+delete";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // ConsumePastBlockDeleteConsumeConcurrent:
    // co_await consume(past_entry) -> delete -> insert(new) -> co_await
    // consume(self) -> co_await consume(concurrent)
    builder.add<SerializationFormat::kTxs>(
        22,
        [](NativeSyscallContext &ctx, uint32_t past_value,
           uint32_t insert_value,
           uint32_t concurrent_value) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_consume_expect(ctx, past_value)) {
            ADD_FAILURE() << "Failed to consume past block entry "
                          << past_value;
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_insert(ctx, insert_value)) {
            ADD_FAILURE() << "Failed to insert";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, insert_value)) {
            ADD_FAILURE() << "Failed to consume self entry " << insert_value;
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (!co_await rcq_consume_expect(ctx, concurrent_value)) {
            ADD_FAILURE() << "Failed to consume concurrent entry "
                          << concurrent_value;
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // ChangeTypeAndRecreateFromEmpty:
    // co_await consume(retrocausal) -> delete -> create bytestring -> delete ->
    // create nnint -> delete -> insert(item)
    builder.add<SerializationFormat::kTxs>(
        23,
        [](NativeSyscallContext &ctx, uint32_t consume_value,
           uint32_t insert_value) -> x23::Async<InvokeStatus<void>> {
          // consume retrocausal item
          if (!co_await rcq_consume_expect(ctx, consume_value)) {
            ADD_FAILURE() << "Failed to consume retrocausal item "
                          << consume_value;
            co_return InvokeError{InvokeErrorCode::None};
          }

          // delete
          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue (first)";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // create bytestring
          Bytestring bytestring =
              NativeContractTestFramework::make_bytestring("test");
          auto r3 = co_await ctx.syscall<SyscallNo::kBytestringSet>(
              kStorageKey, std::as_bytes(std::span(bytestring)),
              std::to_underlying(StorageDeltaFlags::None));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to create bytestring";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // delete
          auto r4 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete bytestring";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // create nnint
          int128_t set_value = 42;
          auto r5 = co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
              kStorageKey, &set_value, 0,
              std::to_underlying(StorageDeltaFlags::None));
          if (!r5.has_value() ||
              r5.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to create nnint";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // delete
          auto r6 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r6.has_value() ||
              r6.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete nnint";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // insert item
          if (!co_await rcq_insert(ctx, insert_value)) {
            ADD_FAILURE() << "Failed to insert item";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 24: peek_only(value) — peek and verify value matches
    builder.add<SerializationFormat::kTxs>(
        24,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          uint32_t peeked = 0;
          RuntimeMemoryLength peek_len = 0;
          auto rp = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, true, std::as_writable_bytes(std::span(&peeked, 1)),
              peek_len);
          if (!rp.has_value() ||
              rp.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to peek";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (peek_len != 4 || peeked != value) {
            ADD_FAILURE() << "Peek: expected " << value << " but got "
                          << peeked;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    // Method 25: delete_and_fail — delete then return error to revert
    builder.add<SerializationFormat::kTxs>(
        25, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto r1 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeError{InvokeErrorCode::Deterministic};
        });

    // Method 26: try_delete_revert_then_consume(expected_value)
    // invoke(method 25, TryInvoke) -> co_await consume(expected_value)
    builder.add<SerializationFormat::kTxs>(
        26,
        [](NativeSyscallContext &ctx,
           uint32_t expected_value) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 25,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran (so the syscall returns None); it must have rolled
          // back (revert or fault) rather than committed.
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // queue should be intact after reverted delete
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(expected_value),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume after reverted delete";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 27: peek_peek_consume_consume(peek_val, consume_a, consume_b)
    builder.add<SerializationFormat::kTxs>(
        27,
        [](NativeSyscallContext &ctx, uint32_t peek_val, uint32_t consume_a,
           uint32_t consume_b) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_peek_expect(ctx, peek_val)) {
            ADD_FAILURE() << "Failed first peek of " << peek_val;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_peek_expect(ctx, peek_val)) {
            ADD_FAILURE() << "Failed second peek of " << peek_val;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_consume_expect(ctx, consume_a)) {
            ADD_FAILURE() << "Failed to co_await consume(a) " << consume_a;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_consume_expect(ctx, consume_b)) {
            ADD_FAILURE() << "Failed to co_await consume(b) " << consume_b;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 29: insert_try_delete_revert_consume_consume(self_val,
    // concurrent_val)
    builder.add<SerializationFormat::kTxs>(
        29,
        [](NativeSyscallContext &ctx, uint32_t self_val,
           uint32_t concurrent_val) -> x23::Async<InvokeStatus<void>> {
          // insert(self_val)
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(self_val),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert self entry";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // try_invoke delete (reverted via inner error)
          CopyableInvokeOutputs r2_outputs{};
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 25,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, r2_outputs);
          }();
          if (!r2.has_value()) {
            ADD_FAILURE() << "System error on try_invoke";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran (so the syscall returns None); it must have rolled
          // back (revert or fault) rather than committed.
          if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r2_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // co_await consume(self_val) - self entries consumed first
          auto r3 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(self_val),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume self entry";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // co_await consume(concurrent_val)
          auto r4 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(concurrent_val),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume concurrent entry";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 30: delete_insert_insert(val_a, val_b)
    builder.add<SerializationFormat::kTxs>(
        30,
        [](NativeSyscallContext &ctx, uint32_t val_a,
           uint32_t val_b) -> x23::Async<InvokeStatus<void>> {
          auto r1 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_insert(ctx, val_a)) {
            ADD_FAILURE() << "Failed to insert(a)";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_insert(ctx, val_b)) {
            ADD_FAILURE() << "Failed to insert(b)";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 31: just_delete — only deletes, no error return
    builder.add<SerializationFormat::kTxs>(
        31, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto r1 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    // Method 32: try_invoke_delete_then_consume(expected_value)
    // invoke(method 31, TryInvoke), expect conflict, consume
    builder.add<SerializationFormat::kTxs>(
        32,
        [](NativeSyscallContext &ctx,
           uint32_t expected_value) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 31,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran (so the syscall returns None); expect it to have
          // rolled back (lock conflict) rather than committed.
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // Expect conflict when concurrent insert exists
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "try_invoke delete should have conflicted";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // queue still intact
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(expected_value),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume after conflicted delete";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 33: consume_delete_peek(old_val, peek_val)
    builder.add<SerializationFormat::kTxs>(
        33,
        [](NativeSyscallContext &ctx, uint32_t old_val,
           uint32_t peek_val) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_consume_expect(ctx, old_val)) {
            ADD_FAILURE() << "Failed to consume old entry " << old_val;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_peek_expect(ctx, peek_val)) {
            ADD_FAILURE() << "Failed to peek " << peek_val << " after delete";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 28: consume_delete_insert_peek(old_val, new_val)
    builder.add<SerializationFormat::kTxs>(
        28,
        [](NativeSyscallContext &ctx, uint32_t old_val,
           uint32_t new_val) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_consume_expect(ctx, old_val)) {
            ADD_FAILURE() << "Failed to consume old entry " << old_val;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_delete(ctx)) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_insert(ctx, new_val)) {
            ADD_FAILURE() << "Failed to insert new entry";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_peek_expect(ctx, new_val)) {
            ADD_FAILURE() << "Failed to peek new entry " << new_val;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 34: insert_and_fail(value) — insert then error (for inner frame
    // revert)
    builder.add<SerializationFormat::kTxs>(
        34,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_insert(ctx, value)) {
            ADD_FAILURE() << "Failed to insert in insert_and_fail";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeError{InvokeErrorCode::Deterministic};
        });

    // Method 35: consume_and_fail(value) — consume then error (for inner frame
    // revert)
    builder.add<SerializationFormat::kTxs>(
        35,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          if (!co_await rcq_consume_expect(ctx, value)) {
            ADD_FAILURE() << "Failed to consume " << value
                          << " in consume_and_fail";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeError{InvokeErrorCode::Deterministic};
        });

    // Method 36: inner_insert_revert_then_consume_empty(value)
    builder.add<SerializationFormat::kTxs>(
        36,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 34,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(value), std::span<uint32_t>{},
                inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke insert";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran (so the syscall returns None); it must have rolled
          // back (revert or fault) rather than committed.
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // queue should be empty after reverted insert
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 2,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume_empty after reverted insert";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 37: inner_insert_revert_then_insert_consume(revert_val, real_val)
    builder.add<SerializationFormat::kTxs>(
        37,
        [](NativeSyscallContext &ctx, uint32_t revert_val,
           uint32_t real_val) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 34,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(revert_val),
                std::span<uint32_t>{}, inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke insert";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // successful insert
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(real_val),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert real_val";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // only real_val should be consumable
          auto r3 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(real_val),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume real_val";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 38: inner_consume_revert_then_consume(value)
    builder.add<SerializationFormat::kTxs>(
        38,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 35,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(value), std::span<uint32_t>{},
                inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke consume";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran (so the syscall returns None); it must have rolled
          // back (revert or fault) rather than committed.
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // entry should still be available after reverted consume
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(value), std::span<uint32_t>{},
                inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume after reverted consume";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 39: inner_consume_revert_then_peek(value)
    builder.add<SerializationFormat::kTxs>(
        39,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 35,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(value), std::span<uint32_t>{},
                inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke consume";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran (so the syscall returns None); it must have rolled
          // back (revert or fault) rather than committed.
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // entry should still be visible via peek
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 24,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(value), std::span<uint32_t>{},
                inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to peek after reverted consume";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 40: inner_consume_revert_twice_then_consume(value)
    builder
        .add<SerializationFormat::kTxs>(
            40,
            [](NativeSyscallContext &ctx,
               uint32_t value) -> x23::Async<InvokeStatus<void>> {
              // first reverted consume
              CopyableInvokeOutputs r1_outputs{};
              auto r1 =
                  co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
                CopyableInvokeInputs inputs{
                    .entrypoint_id = 35,
                    .gas_limit = std::numeric_limits<GasAmount>::max(),
                    .flags = std::to_underlying(InvokeFlag::TryInvoke),
                };
                co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                    kContractAddress, x23::serialize(value),
                    std::span<uint32_t>{}, inputs, r1_outputs);
              }();
              if (!r1.has_value()) {
                ADD_FAILURE() << "System error on try_invoke consume (1st)";
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }
              // The child ran (so the syscall returns None); it must have
              // rolled back (revert or fault) rather than committed.
              if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                ADD_FAILURE()
                    << "try_invoke was rejected before running child (1st)";
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }
              if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
                ADD_FAILURE() << "expected the child frame to fault (1st)";
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }

              // second reverted consume
              CopyableInvokeOutputs r2_outputs{};
              auto r2 =
                  co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
                CopyableInvokeInputs inputs{
                    .entrypoint_id = 35,
                    .gas_limit = std::numeric_limits<GasAmount>::max(),
                    .flags = std::to_underlying(InvokeFlag::TryInvoke),
                };
                co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                    kContractAddress, x23::serialize(value),
                    std::span<uint32_t>{}, inputs, r2_outputs);
              }();
              if (!r2.has_value()) {
                ADD_FAILURE() << "System error on try_invoke consume (2nd)";
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }
              // The child ran (so the syscall returns None); it must have
              // rolled back (revert or fault) rather than committed.
              if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                ADD_FAILURE()
                    << "try_invoke was rejected before running child (2nd)";
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }
              if (r2_outputs.frame_status != InvocationFrameStatusCode::Fault) {
                ADD_FAILURE() << "expected the child frame to fault (2nd)";
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }

              // entry should still be available
              auto r3 =
                  co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
                CopyableInvokeInputs inputs{
                    .entrypoint_id = 1,
                    .gas_limit = std::numeric_limits<GasAmount>::max(),
                    .flags = std::to_underlying(InvokeFlag::None),
                };
                co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                    kContractAddress, x23::serialize(value),
                    std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
              }();
              if (!r3.has_value() ||
                  r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                ADD_FAILURE()
                    << "Failed to consume after two reverted consumes";
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }

              co_return InvokeStatus<void>{};
            });

    // Method 41:
    // inner_consume_insert_revert_then_consume_and_empty(consume_val,
    // insert_val)
    builder.add<SerializationFormat::kTxs>(
        41,
        [](NativeSyscallContext &ctx, uint32_t consume_val,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          // try_invoke: consume + insert, both reverted
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 42,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(consume_val, insert_val),
                std::span<uint32_t>{}, inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke consume+insert";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran (so the syscall returns None); it must have rolled
          // back (revert or fault) rather than committed.
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // original entry still available
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(consume_val),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume original entry";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // reverted insert should be gone
          auto r3 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 2,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume_empty after reverted insert";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 42: consume_insert_and_fail(consume_val, insert_val) — consume
    // then insert then error (for inner frame revert)
    builder.add<SerializationFormat::kTxs>(
        42,
        [](NativeSyscallContext &ctx, uint32_t consume_val,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          // Runs as the TryInvoke'd child, so its own operations are in-frame:
          // invoking this contract again from here would be reentrant even
          // with an orchestrator driving the outer frame.
          if (!co_await rcq_consume_expect(ctx, consume_val)) {
            ADD_FAILURE() << "Failed to consume " << consume_val
                          << " in consume_insert_and_fail";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await rcq_insert(ctx, insert_val)) {
            ADD_FAILURE() << "Failed to insert in consume_insert_and_fail";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeError{InvokeErrorCode::Deterministic};
        });

    // Method 43: inner_insert_revert_then_consume(revert_val, concurrent_val)
    builder.add<SerializationFormat::kTxs>(
        43,
        [](NativeSyscallContext &ctx, uint32_t revert_val,
           uint32_t concurrent_val) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs r1_outputs{};
          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 34,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(revert_val),
                std::span<uint32_t>{}, inputs, r1_outputs);
          }();
          if (!r1.has_value()) {
            ADD_FAILURE() << "System error on try_invoke insert";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke was rejected before running child";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r1_outputs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the child frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // consume concurrent entry (unaffected by reverted insert)
          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = std::numeric_limits<GasAmount>::max(),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kContractAddress, x23::serialize(concurrent_val),
                std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          }();
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume concurrent entry";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 44: delete_queue_create_bytestring_delete_bytestring_consume
    // (concurrent_val)
    // Delete queue -> create bytestring -> delete bytestring -> consume
    builder.add<SerializationFormat::kTxs>(
        44,
        [](NativeSyscallContext &ctx,
           uint32_t concurrent_val) -> x23::Async<InvokeStatus<void>> {
          // delete queue
          auto r1 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // create bytestring
          Bytestring bytestring =
              NativeContractTestFramework::make_bytestring("test");
          auto r2 = co_await ctx.syscall<SyscallNo::kBytestringSet>(
              kStorageKey, std::as_bytes(std::span(bytestring)),
              std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to create bytestring";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // delete bytestring
          auto r3 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete bytestring";
            co_return InvokeError{InvokeErrorCode::None};
          }

          // consume concurrent entry -- the consume should succeed
          if (!co_await rcq_consume_expect(ctx, concurrent_val)) {
            ADD_FAILURE() << "Consume concurrent entry " << concurrent_val
                          << " should have succeeded";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 45: delete_queue_create_bytestring_consume(concurrent_val)
    // Delete queue -> create bytestring -> consume
    // consume should fail, so bytestring should be left
    builder.add<SerializationFormat::kTxs>(
        45, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          // delete queue
          auto r1 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // create bytestring
          Bytestring bytestring =
              NativeContractTestFramework::make_bytestring("test");
          auto r2 = co_await ctx.syscall<SyscallNo::kBytestringSet>(
              kStorageKey, std::as_bytes(std::span(bytestring)),
              std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to create bytestring";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // consume concurrent entry -- the consume should fail because the
          // bytestring exists there
          if (!co_await rcq_consume_expect_failure(ctx)) {
            ADD_FAILURE() << "Consume should have failed over the bytestring";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // ConsumeAll: drain the queue until empty (single TX).
    // Buffer is max entry size (kQueueEntryMaxLen = 256) so this works
    // regardless of how entries were inserted.
    builder.add<SerializationFormat::kTxs>(
        50, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          while (true) {
            std::array<uint8_t, 256> out{};
            RuntimeMemoryLength written_len = 0;
            auto res =
                co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                    kStorageKey, false, std::as_writable_bytes(std::span(out)),
                    written_len);
            if (!res.has_value()) {
              ADD_FAILURE() << "System error consuming entry";
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }
            if (res.value() == SyscallErrorCode::
                                   kSyscallErrorCodeNoRetrocausalQueueEntry ||
                res.value() ==
                    SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
              break;
            }
            if (res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "Unexpected error consuming entry";
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }
          }
          co_return InvokeStatus<void>{};
        });

    // Method 51: delete -> peek(expect nothing) -> co_await consume(expect
    // nothing)
    builder.add<SerializationFormat::kTxs>(
        51, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto r1 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            co_return InvokeError{InvokeErrorCode::None};
          }

          uint32_t peeked = 0;
          RuntimeMemoryLength peek_len = 0;
          auto r2 = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, true, std::as_writable_bytes(std::span(&peeked, 1)),
              peek_len);
          if (!r2.has_value() ||
              ((r2.value() !=
                SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) &&
               (r2.value() !=
                SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist))) {
            ADD_FAILURE() << "Peek should not find an entry";
            co_return InvokeError{InvokeErrorCode::None};
          }
          if (peek_len != 0) {
            ADD_FAILURE() << "Peek wrote output for an empty queue";
            co_return InvokeError{InvokeErrorCode::None};
          }

          if (!co_await rcq_consume_expect_empty(ctx)) {
            ADD_FAILURE() << "consume after peek did not come back empty";
            co_return InvokeError{InvokeErrorCode::None};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 60: consume_delete_insert_direct(consume_val, insert_val)
    // Uses direct syscalls only (no inner kInvoke), so it works on any address.
    builder.add<SerializationFormat::kTxs>(
        60,
        [](NativeSyscallContext &ctx, uint32_t consume_val,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          uint32_t out = 0;
          RuntimeMemoryLength written_len = 0;
          auto r1 = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
              written_len);
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "consume failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (written_len != 4 || out != consume_val) {
            ADD_FAILURE() << "consume: expected " << consume_val << " got "
                          << out;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "delete failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r3 = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
              kStorageKey, std::as_bytes(std::span(&insert_val, 1)));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "insert failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 61: consume_consume_delete_insert_direct(val_a, val_b, insert_val)
    // Consumes two concurrent entries, deletes, inserts. Direct syscalls only.
    builder.add<SerializationFormat::kTxs>(
        61,
        [](NativeSyscallContext &ctx, uint32_t val_a, uint32_t val_b,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          for (uint32_t expected : {val_a, val_b}) {
            uint32_t out = 0;
            RuntimeMemoryLength written_len = 0;
            auto r = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
                written_len);
            if (!r.has_value() ||
                r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "consume failed";
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }
            if (written_len != 4 || out != expected) {
              ADD_FAILURE()
                  << "consume: expected " << expected << " got " << out;
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }
          }

          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "delete failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r3 = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
              kStorageKey, std::as_bytes(std::span(&insert_val, 1)));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "insert failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 62: consume_delete_consume_delete_insert_direct(val_a, val_b,
    // insert_val)
    // Two delete/recreate cycles. Direct syscalls only.
    builder.add<SerializationFormat::kTxs>(
        62,
        [](NativeSyscallContext &ctx, uint32_t val_a, uint32_t val_b,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          auto consume_one = [&](uint32_t expected) -> x23::Async<bool> {
            uint32_t out = 0;
            RuntimeMemoryLength written_len = 0;
            auto r = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
                written_len);
            if (!r.has_value() ||
                r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "consume failed";
              co_return false;
            }
            if (written_len != 4 || out != expected) {
              ADD_FAILURE()
                  << "consume: expected " << expected << " got " << out;
              co_return false;
            }
            co_return true;
          };
          auto delete_queue = [&]() -> x23::Async<bool> {
            auto r = co_await ctx.syscall<SyscallNo::kDeleteObject>(
                kStorageKey, std::to_underlying(StorageDeltaFlags::None));
            if (!r.has_value() ||
                r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "delete failed";
              co_return false;
            }
            co_return true;
          };
          auto insert_one = [&](uint32_t val) -> x23::Async<bool> {
            auto r = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
                kStorageKey, std::as_bytes(std::span(&val, 1)));
            if (!r.has_value() ||
                r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "insert failed";
              co_return false;
            }
            co_return true;
          };

          // co_await consume(a) -> delete -> insert(b) -> co_await consume(b)
          // -> delete -> insert(final) After first delete-recreate,
          // self-entries are consumed before concurrent, so consume gets the
          // just-inserted val_b.
          if (!co_await consume_one(val_a)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await delete_queue()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await insert_one(val_b)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await consume_one(val_b)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await delete_queue()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await insert_one(insert_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 63: peek_delete_insert_consume_direct(peek_val, insert_val)
    // Peeks a concurrent entry, deletes queue, inserts new entry, then
    // consumes twice: first the self-inserted entry, then the original
    // concurrent entry (which survives because peek didn't advance the
    // cursor, so retrocausally_consumed_count stays 0).
    builder.add<SerializationFormat::kTxs>(
        63,
        [](NativeSyscallContext &ctx, uint32_t peek_val,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          auto consume = [&](bool peek, uint32_t expected) -> x23::Async<bool> {
            uint32_t out = 0;
            RuntimeMemoryLength written_len = 0;
            auto r = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                kStorageKey, peek, std::as_writable_bytes(std::span(&out, 1)),
                written_len);
            if (!r.has_value() ||
                r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << (peek ? "peek" : "consume") << " failed";
              co_return false;
            }
            if (written_len != 4 || out != expected) {
              ADD_FAILURE() << (peek ? "peek" : "consume") << ": expected "
                            << expected << " got " << out;
              co_return false;
            }
            co_return true;
          };

          if (!co_await consume(true, peek_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "delete failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r3 = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
              kStorageKey, std::as_bytes(std::span(&insert_val, 1)));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "insert failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          if (!co_await consume(false, insert_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await consume(false, peek_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 64: peek_only_delete_insert_direct(peek_val, insert_val)
    // Peeks (no consume) then deletes and inserts. Peek doesn't advance the
    // cursor so retrocausally_consumed_count stays 0 and the concurrent entry
    // survives the delete/recreate.
    builder.add<SerializationFormat::kTxs>(
        64,
        [](NativeSyscallContext &ctx, uint32_t peek_val,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          uint32_t out = 0;
          RuntimeMemoryLength written_len = 0;
          auto r1 = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kStorageKey, true, std::as_writable_bytes(std::span(&out, 1)),
              written_len);
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "peek failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (written_len != 4 || out != peek_val) {
            ADD_FAILURE() << "peek: expected " << peek_val << " got " << out;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "delete failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r3 = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
              kStorageKey, std::as_bytes(std::span(&insert_val, 1)));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "insert failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 65: insert_consume_self_delete_insert_consume_concurrent(
    //     self_val, insert_val, concurrent_val)
    // Inserts self entry, consumes it, deletes, inserts new, then consumes
    // a concurrent entry. Verifies self_consumed_index doesn't interfere
    // with retrocausally_consumed_count through a delete/recreate.
    builder.add<SerializationFormat::kTxs>(
        65,
        [](NativeSyscallContext &ctx, uint32_t self_val, uint32_t insert_val,
           uint32_t concurrent_val) -> x23::Async<InvokeStatus<void>> {
          auto do_insert = [&](uint32_t val) -> x23::Async<bool> {
            auto r = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
                kStorageKey, std::as_bytes(std::span(&val, 1)));
            if (!r.has_value() ||
                r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "insert failed";
              co_return false;
            }
            co_return true;
          };
          auto do_consume = [&](uint32_t expected) -> x23::Async<bool> {
            uint32_t out = 0;
            RuntimeMemoryLength written_len = 0;
            auto r = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                kStorageKey, false, std::as_writable_bytes(std::span(&out, 1)),
                written_len);
            if (!r.has_value() ||
                r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              ADD_FAILURE() << "consume failed";
              co_return false;
            }
            if (written_len != 4 || out != expected) {
              ADD_FAILURE()
                  << "consume: expected " << expected << " got " << out;
              co_return false;
            }
            co_return true;
          };

          // insert(self_val) -> co_await consume(self_val) -> delete ->
          // insert(insert_val) -> co_await consume(insert_val) ->
          // co_await consume(concurrent_val)
          if (!co_await do_insert(self_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await do_consume(self_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r2 = co_await ctx.syscall<SyscallNo::kDeleteObject>(
              kStorageKey, std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "delete failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          if (!co_await do_insert(insert_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await do_consume(insert_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!co_await do_consume(concurrent_val)) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });

    // Method 67 (issue #3152 reachability, multi-frame): co_await
    // consume(concurrent) -> delete -> insert(different), with the consume and
    // insert each in their own child frame. Because deltas are flushed to the
    // DB only at a frame boundary (try_push_new_subtransaction), the delete
    // lands as its own subtransaction before the recreate, so HogDb sees a
    // deletion -> RCQ exclusive upgrade. The recreated RCQ reloads the consumed
    // concurrent entry into current_block_consumed (via
    // make_fully_consumed_item) while added_entries holds only the new insert
    // -- the exact shape HogDb's RCQ ExclusiveLock rejects as
    // kInvalidExecutorBehavior. Unlike methods 62-65 (single frame, so
    // delete+recreate fold into one delta) this actually splits them across a
    // push.
    builder.add<SerializationFormat::kTxs>(
        67,
        [](NativeSyscallContext &ctx, uint32_t concurrent_val,
           uint32_t insert_val) -> x23::Async<InvokeStatus<void>> {
          // Consume the concurrent entry in a child frame (entrypoint 1).
          CopyableInvokeInputs consume_in{
              .entrypoint_id = 1,
              .gas_limit = std::numeric_limits<GasAmount>::max(),
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto r1 = co_await ctx.syscall<SyscallNo::kInvoke>(
              kContractAddress, x23::serialize(concurrent_val),
              std::span<uint32_t>{}, consume_in, CopyableInvokeOutputs{});
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // Delete in a child frame (entrypoint 31, which only deletes). The
          // deletion delta propagates up into this frame when that child
          // returns, so it is pending here when the insert child below
          // returns -- which is the collision this test covers. Doing the
          // delete in this frame directly would work too, but would force
          // this frame to be kContractAddress and so make the insert child
          // reentrant.
          CopyableInvokeInputs delete_in{
              .entrypoint_id = 31,
              .gas_limit = std::numeric_limits<GasAmount>::max(),
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto r2 = co_await ctx.syscall<SyscallNo::kInvoke>(
              kContractAddress, std::span<const std::byte>{},
              std::span<uint32_t>{}, delete_in, CopyableInvokeOutputs{});
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // Insert a DIFFERENT entry in a child frame (entrypoint 0). Crossing
          // this boundary first flushes the deletion subtransaction.
          CopyableInvokeInputs insert_in{
              .entrypoint_id = 0,
              .gas_limit = std::numeric_limits<GasAmount>::max(),
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto r3 = co_await ctx.syscall<SyscallNo::kInvoke>(
              kContractAddress, x23::serialize(insert_val),
              std::span<uint32_t>{}, insert_in, CopyableInvokeOutputs{});
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    // Method 68: TryInvoke a *successful* insert into kContractAddress3, then
    // fail this frame. The TryInvoke succeeded on its own terms, so its writes
    // were probed and committed into this frame -- and this frame then
    // reverts, which must discard them.
    builder.add<SerializationFormat::kTxs>(
        68,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs outs{};
          CopyableInvokeInputs in{
              .entrypoint_id = 0,
              .gas_limit = std::numeric_limits<GasAmount>::max(),
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          auto r =
              co_await ctx.syscall<SyscallNo::kInvoke>(kContractAddress3,
                                                       x23::serialize(value),
                                                       std::span<uint32_t>{},
                                                       in, outs);
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "System error on nested try_invoke insert";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (outs.frame_status != InvocationFrameStatusCode::Return) {
            ADD_FAILURE() << "nested try_invoke insert should have succeeded";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // Revert this frame, discarding the insert the TryInvoke committed
          // into it.
          co_return InvokeError{InvokeErrorCode::Deterministic};
        });

    // Method 69: orchestrator for 68. Invokes it and expects it to fault.
    builder.add<SerializationFormat::kTxs>(
        69,
        [](NativeSyscallContext &ctx,
           uint32_t value) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs outs{};
          CopyableInvokeInputs in{
              .entrypoint_id = 68,
              .gas_limit = std::numeric_limits<GasAmount>::max(),
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto r =
              co_await ctx.syscall<SyscallNo::kInvoke>(kContractAddress,
                                                       x23::serialize(value),
                                                       std::span<uint32_t>{},
                                                       in, outs);
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "System error invoking the reverting frame";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (outs.frame_status != InvocationFrameStatusCode::Fault) {
            ADD_FAILURE() << "expected the reverting frame to fault";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    // Method 70: try_invoke_child(child_entrypoint, value, expect_fault).
    // TryInvokes child_entrypoint(value) at kContractAddress and checks the
    // frame outcome: Return means the lock probe committed the child's writes
    // into this frame; Fault (expect_fault != 0) means the probe conflicted
    // and rolled the child back.
    builder.add<SerializationFormat::kTxs>(
        70,
        [](NativeSyscallContext &ctx, uint32_t child_entrypoint, uint32_t value,
           uint32_t expect_fault) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeOutputs outs{};
          CopyableInvokeInputs in{
              .entrypoint_id = child_entrypoint,
              .gas_limit = std::numeric_limits<GasAmount>::max(),
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          auto r =
              co_await ctx.syscall<SyscallNo::kInvoke>(kContractAddress,
                                                       x23::serialize(value),
                                                       std::span<uint32_t>{},
                                                       in, outs);
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "System error on try_invoke";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto expected = expect_fault != 0 ? InvocationFrameStatusCode::Fault
                                            : InvocationFrameStatusCode::Return;
          if (outs.frame_status != expected) {
            ADD_FAILURE() << "try_invoke child entrypoint " << child_entrypoint
                          << ": expected "
                          << (expect_fault != 0 ? "Fault" : "Return");
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, builder.build());
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  constexpr static ContractAddress kContractAddress2 = make_contract_address(1);
  // Third address, so a frame can TryInvoke a contract that is neither itself
  // nor the orchestrator driving it.
  constexpr static ContractAddress kContractAddress3 = make_contract_address(2);

  static AddressAndKey get_queue_addrkey_for(ContractAddress const &addr)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = kStorageKey;
    return AddressAndKey{.address = addr, .key = key};
  }

  void SetUp() override
  {
    // mark the contract as existing and deployed within the database
    auto contract_0 = make_native_contract(0);
    write_contract(contract_0);
    write_deployment(kContractAddress, contract_0);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
    write_deployment(kContractAddress2, contract_0);
    setup_grant_storage_token(kContractAddress2, 1'000'000'000);
    write_deployment(kContractAddress3, contract_0);
    setup_grant_storage_token(kContractAddress3, 1'000'000'000);
    context_ = make_context();

    wait_flag_1_ = false;
    wait_flag_2_ = false;
  }

  template<x23::is_xdr_type... Args>
  void invoke_method(Runtime::MethodID method, Args const &...args)
  {
    invoke_method_impl(kContractAddress, method, args...);
  }

  // Runs `method` at kContractAddress2 instead, as an orchestrator.
  //
  // A note on what these tests do and do not cover. There are two kinds of
  // TryInvoke'd child here:
  //
  //  - Children 25, 34, 35 and 42 do their RCQ work and then fail on purpose.
  //    InvocationFrameStack::finish_frame only takes its TryInvoke path when
  //    the frame *succeeds* and was not manually reverted, so for these the
  //    flag is inert: the rollback observed is frame-failure rollback, and
  //    removing InvokeFlag::TryInvoke does not change the outcome.
  //
  //  - Children that succeed in-frame are the case TryInvoke actually
  //    governs: the lock probe for their writes runs and either commits them
  //    into the caller or rewrites the frame result to Fault. Child 31
  //    (driven by method 32), children 0 and 1 (driven by method 70), and
  //    child 0 at kContractAddress3 (driven by method 68) are all of this
  //    kind; the tests built on them fail if the flag is removed.
  //
  // The methods that drive a TryInvoke'd child frame do no storage work
  // themselves -- every RCQ operation happens in a frame they invoke at
  // kContractAddress (method 68's TryInvoke'd insert is the one exception:
  // it targets kContractAddress3 so the reverting frame in between is not
  // reentrant). Running the orchestrating frame at a second address keeps
  // those operations off the orchestrator's queue while never putting the
  // same address on the invocation stack twice.
  //
  // The children must stay real invokes for this: they are what the TryInvoke
  // reverts, and they have to execute at kContractAddress so they act on that
  // queue rather than the orchestrator's.
  template<x23::is_xdr_type... Args>
  void invoke_method_orchestrated(Runtime::MethodID method, Args const &...args)
  {
    invoke_method_impl(kContractAddress2, method, args...);
  }

  template<x23::is_xdr_type... Args>
  void invoke_method_impl(ContractAddress const &root_address,
                          Runtime::MethodID method, Args const &...args)
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = root_address,
                                   .entrypoint =
                                       entrypoint_from_method_id(method),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = x23::serialize(args...)},
                    .uniquifier = make_new_uniquifier(),
                }},
        *context_);

    auto r = handler.root_invoke()();
    ASSERT_TRUE(r.is_root_success());
    auto res = handler.try_commit_transaction()();
    ASSERT_EQ(res, ExecResult::ExecResultOK);
  }

  template<x23::is_xdr_type... Args>
  void invoke_method_fail_commit(Runtime::MethodID method, Args const &...args)
  {
    invoke_method_fail_commit_impl(kContractAddress, method, args...);
  }

  template<x23::is_xdr_type... Args>
  void invoke_method_fail_commit_impl(ContractAddress const &root_address,
                                      Runtime::MethodID method,
                                      Args const &...args)
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = root_address,
                                   .entrypoint =
                                       entrypoint_from_method_id(method),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = x23::serialize(args...)},
                    .uniquifier = make_new_uniquifier(),
                }},
        *context_);

    auto r = handler.root_invoke()();
    ASSERT_TRUE(r.is_root_success());
    auto res = handler.try_commit_transaction()();
    ASSERT_NE(res, ExecResult::ExecResultOK);
  }

  // Transaction builder for tests that drive SystemCallHandler directly
  // (the ProposalValidation tests) instead of going through
  // invoke_method_impl. Several older tests still carry a local lambda with
  // this exact shape; new tests should use this member.
  template<x23::is_xdr_type... Args>
  Transaction make_tx(ContractAddress const &addr, Runtime::MethodID method,
                      Args const &...args)
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  }

  void insert(uint32_t value) { invoke_method(0, value); }

  void set_bytestring(Bytestring const &bytestring)
  {
    invoke_method(3, bytestring);
  }

  testing::AssertionResult check_rcq_empty_for(ContractAddress const &addr)
  {
    auto queue = lookup_storage_item_from_db(get_queue_addrkey_for(addr));
    if (GetParam() == TestDB::kHogDb) {
      if (queue.has_value()) {
        return testing::AssertionFailure() << "Queue has live entries";
      }
      return testing::AssertionSuccess();
    }

    if (!queue.has_value()) {
      return testing::AssertionFailure() << "No queue found";
    }
    if (queue->type() != StorageItemType::kItemTypeRetrocausalQueue) {
      return testing::AssertionFailure() << "Queue has wrong type";
    }
    auto const &qe = queue->metadata();
    if (qe.live_entries != 0) {
      return testing::AssertionFailure() << "Queue has live entries";
    }
    return testing::AssertionSuccess();
  }

  testing::AssertionResult check_rcq_empty()
  {
    // The orchestrated tests depend on kContractAddress2 doing no storage work
    // of its own: a stray in-frame RCQ call in one of those methods would act
    // on the orchestrator's queue, and every assertion here inspects
    // kContractAddress's. kContractAddress3 gets the same guard: the only
    // method that writes there (68) always reverts, so a committed queue at
    // that address is likewise a stray. Checked from here rather than from
    // invoke_method_orchestrated because DB reads only see the write after
    // advance_block(), which callers do before reaching this.
    for (auto const &aux_addr : {kContractAddress2, kContractAddress3}) {
      if (lookup_storage_item_from_db(get_queue_addrkey_for(aux_addr))
              .has_value()) {
        return testing::AssertionFailure()
               << "auxiliary address has a queue: a method meant to only "
                  "invoke (or revert) did RCQ work that reached the DB";
      }
    }

    auto queue = lookup_storage_item_from_db(get_queue_addrkey());
    if (GetParam() == TestDB::kHogDb) {
      // On HogDb, metadata is removed when live_entries drops to 0.
      if (queue.has_value()) {
        return testing::AssertionFailure() << "Queue has live entries";
      }
      return testing::AssertionSuccess();
    }

    if (!queue.has_value()) {
      return testing::AssertionFailure() << "No queue found";
    }
    if (queue->type() != StorageItemType::kItemTypeRetrocausalQueue) {
      return testing::AssertionFailure() << "Queue has wrong type";
    }
    auto const &qe = queue->metadata();
    if (qe.live_entries != 0) {
      return testing::AssertionFailure() << "Queue has live entries";
    }
    return testing::AssertionSuccess();
  }

  std::unique_ptr<RuntimeContext> context_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_P(RCQDeleteTest, NoDoubleConsumeDeletedEntries)
{
  insert(42);
  insert(99);
  invoke_method(10, uint32_t{42}, uint32_t{99});

  advance_block();

  // TODO(geoff): assertion on qe.max_live_block_num
  ASSERT_TRUE(check_rcq_empty());
}

TEST_P(RCQDeleteTest, NoDeleteUnobservedEntries)
{
  std::thread t([&] { invoke_method(11, uint32_t{42}); });

  wait_flag_1_.wait(false);

  insert(42);

  wait_flag_2_ = true;
  wait_flag_2_.notify_all();
  t.join();

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

TEST_P(RCQDeleteTest, DeleteSelfInsertedEntry)
{
  insert(42);

  // insert -> delete -> consume
  invoke_method(12, uint32_t{99}, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

TEST_P(RCQDeleteTest, DeleteAndRecreateRemovesPastBlocks)
{
  insert(42);
  insert(99);

  advance_block();

  invoke_method(13, uint32_t{10});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);
  ASSERT_EQ(qe.max_live_block_num,
            2); // TODO(geoff): is this assertion correct?   This matches in
                // hog_db and mockdb, so I guess so?
}

// Verifies that when an RCQ with entries in past blocks is deleted and
// recreated as a new RCQ in the same transaction (Case 1: snapshot RCQ →
// committed RCQ), the old block records are removed from RAMDB.
//
// This is the case where hog_exec's make_fully_consumed_item must fold a
// consume-all into the new RCQ delta. If hog_exec ever regressed and stopped
// doing this, the old block records would be orphaned as ghost entries.
// DeleteAndRecreateRemovesPastBlocks verifies the metadata is correct but
// doesn't check the block records directly.
TEST_P(RCQDeleteTest, DeleteAndRecreateRemovesBlockRecords)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "Block record check requires HogDb DAO access";
  }

  // Block 1: insert entries that become a past block
  insert(10);
  insert(20);

  advance_block();

  // Block 2: insert another entry (creates a second past block)
  insert(30);

  advance_block();

  // Sanity: the RCQ exists with 3 entries across 2 blocks.
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 3);

  // Get DAO access to check block records directly.
  auto &dao = get_hog_db_dao();
  auto item_key = make_item_key(get_queue_addrkey());

  // Verify blocks exist before delete-and-recreate.
  auto block1 = dao.read_retrocausal_queue_block(item_key, 1);
  ASSERT_TRUE(block1.has_value()) << "Block 1 should exist before delete";
  auto block2 = dao.read_retrocausal_queue_block(item_key, 2);
  ASSERT_TRUE(block2.has_value()) << "Block 2 should exist before delete";

  // Block 3: delete -> insert (same tx). This is the RCQ->delete->RCQ path.
  // hog_exec should fold a consume-all via make_fully_consumed_item so that
  // RetrocausalQueueLock::stage_committed_changes prunes old blocks.
  invoke_method(13, uint32_t{99});

  advance_block();

  // New queue should have exactly 1 entry.
  auto after = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(after.has_value());
  ASSERT_EQ(after->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(after->metadata().live_entries, 1);

  // Old block records must be gone. If hog_exec forgets to issue the
  // consume-all, these would still exist as ghost entries.
  auto block1_after = dao.read_retrocausal_queue_block(item_key, 1);
  EXPECT_FALSE(block1_after.has_value())
      << "Block 1 should be removed after delete-and-recreate (hog_exec "
         "consume-all regression)";
  auto block2_after = dao.read_retrocausal_queue_block(item_key, 2);
  EXPECT_FALSE(block2_after.has_value())
      << "Block 2 should be removed after delete-and-recreate (hog_exec "
         "consume-all regression)";

  // Consume the new entry to verify it's the new one (99).
  invoke_method(1, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Consume 1 of 3 concurrent entries, delete, consume remaining 2.
// Exposes the bug where current_block_consumed lost entries across delete.
TEST_P(RCQDeleteTest, ConsumeOneOfManyDeleteConsumeRest)
{
  insert(42);
  insert(99);
  insert(77);
  invoke_method(14, uint32_t{42}, uint32_t{99}, uint32_t{77});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Consume entry from past block, delete queue, insert new entry,
// consume concurrent entry.
TEST_P(RCQDeleteTest, ConsumePastBlockDeleteConsumeConcurrent)
{
  // Block 1: insert entries that become past block entries
  insert(42);

  advance_block();

  // Block 2: concurrent insert + our method
  insert(99);
  invoke_method(22, uint32_t{42}, uint32_t{10}, uint32_t{99});

  advance_block();

  // Past block entry (42) consumed via make_fully_consumed_item,
  // self insert (10) consumed, concurrent (99) consumed.
  ASSERT_TRUE(check_rcq_empty());
}

// Insert and consume a self entry, delete, insert and consume another self.
// Verifies self entries before delete are discarded correctly.
TEST_P(RCQDeleteTest, InsertSelfConsumeSelfDeleteInsertConsume)
{
  invoke_method(15, uint32_t{42}, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Insert a self entry, delete, then consume a concurrent entry.
// Verifies self insert is gone, concurrent is consumed.
TEST_P(RCQDeleteTest, InsertSelfDeleteConsumeConcurrent)
{
  insert(42);
  invoke_method(12, uint32_t{99}, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Delete queue, then try to consume. Should get no entry.
// Insert is in previous block so no concurrent entries are available.
// The empty consume should leave the key deleted, not convert to empty RCQ.
TEST_P(RCQDeleteTest, DeleteConsumeEmpty)
{
  insert(42);

  advance_block();

  invoke_method(17);

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  EXPECT_FALSE(queue.has_value());
}

// Consume (nothing available) -> delete -> consume (nothing available).
// Both should return no entry. retrocausally_consumed_count_ should remain 0.
// The key should end up deleted, not converted to empty RCQ.
TEST_P(RCQDeleteTest, ConsumeEmptyDeleteConsumeEmpty)
{
  invoke_method(18);

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  EXPECT_FALSE(queue.has_value());
}

// Delete -> peek(empty) -> co_await consume(empty). The peek must not decide
// the final write shape; the empty consume should leave the key deleted.
TEST_P(RCQDeleteTest, DeletePeekEmptyConsumeEmpty)
{
  invoke_method(51);

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  EXPECT_FALSE(queue.has_value());
}

// Multiple delete-recreate cycles: co_await consume(a) -> delete -> co_await
// consume(b) -> delete
// -> co_await consume(c). Verifies that preserved entries accumulate correctly
// across multiple cycles.
TEST_P(RCQDeleteTest, MultipleDeleteRecreate)
{
  insert(42);
  insert(99);
  insert(77);
  invoke_method(19, uint32_t{42}, uint32_t{99}, uint32_t{77});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Two delete-recreate cycles without consumes in between,
// then consume a concurrent entry at the end.
TEST_P(RCQDeleteTest, DeleteInsertDeleteInsertConsume)
{
  insert(42);
  invoke_method(20, uint32_t{10}, uint32_t{20}, uint32_t{42});

  advance_block();

  // Self entry 10 destroyed by second delete, self entry 20 consumed,
  // concurrent 42 consumed.
  ASSERT_TRUE(check_rcq_empty());
}

// Peek at concurrent entry, delete, consume. Since peek doesn't advance
// the cursor, retrocausally_consumed_count_ stays 0. After delete,
// consume should return the same entry (index 0).
TEST_P(RCQDeleteTest, PeekDeleteConsume)
{
  insert(42);
  invoke_method(21, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Start with an empty slot.  Consumes a retrocausal item, then deletes,
// then creates a bytestring, then deletes, then creates an nnint,
// then deletes, then inserts an item.  Verify that at the next block there's
// only the one item inserted at the end
TEST_P(RCQDeleteTest, ChangeTypeAndRecreateFromEmpty)
{
  // Insert an item in a previous block so it becomes retrocausal
  insert(42);

  // In this block: consume retrocausal, delete, create bytestring, delete,
  // create nnint, delete, insert new item
  invoke_method(23, uint32_t{42}, uint32_t{99});

  advance_block();

  // Verify only the inserted item remains
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);

  // Verify that the one item left is the one inserted at the end
  invoke_method(1, uint32_t{99});
}

// Start with a nonempty slot.  Consumes an item, then deletes,
// then creates a bytestring, then deletes, then creates an nnint,
// then deletes, then inserts an item.  Verify that at the next block there's
// only the one item inserted at the end
TEST_P(RCQDeleteTest, ChangeTypeAndRecreateFromNonEmpty)
{
  // Insert an item in a previous block
  insert(42);

  advance_block();

  // In this block: consume retrocausal, delete, create bytestring, delete,
  // create nnint, delete, insert new item
  invoke_method(23, uint32_t{42}, uint32_t{99});

  advance_block();

  // Verify only the inserted item remains
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);

  // Verify that the one item left is the one inserted at the end
  invoke_method(1, uint32_t{99});
}

// Start with a nonempty queue. Delete the item, create a bytestring,
// delete the bytestring, then consume from the queue. A concurrent tx has
// inserted a retrocausal item into the queue.
TEST_P(RCQDeleteTest, DeleteCreateBytestringDeleteBytestringConsume)
{
  // Insert an item in a previous block so queue is nonempty
  insert(42);

  advance_block();

  // Concurrent tx inserts a retrocausal item
  insert(99);

  // In this block: delete queue, create bytestring, delete bytestring,
  // consume concurrent retrocausal item
  invoke_method(44, uint32_t{99});

  advance_block();

  // Verify the concurrent item was consumed
  ASSERT_TRUE(check_rcq_empty());
}

// Start with a nonempty queue. Delete the item, create a bytestring,
// then consume from the queue (without deleting the bytestring). A concurrent
// tx has inserted a retrocausal item into the queue.
TEST_P(RCQDeleteTest, DeleteCreateBytestringConsume)
{
  // Insert an item in a previous block so queue is nonempty
  insert(42);

  advance_block();

  // Concurrent tx inserts a retrocausal item
  insert(99);

  // In this block: delete queue, create bytestring, consume concurrent
  // retrocausal item
  // The last consume should fail, so the bytestring should be left as the last
  // write that write conflicts with the concurrent insert
  invoke_method_fail_commit(45);

  advance_block();

  // Verify that there's a bytestring
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 2);
}

// Start with a nonempty queue. Delete the item, create a bytestring,
// then consume from the queue (without deleting the bytestring)
TEST_P(RCQDeleteTest, DeleteCreateBytestringConsumeSuccess)
{
  // Insert an item in a previous block so queue is nonempty
  insert(42);

  advance_block();

  // As compared to DeleteCreateBytestringConsume, this test does not have a
  // concurrent insert

  // In this block: delete queue, create bytestring, consume concurrent
  // retrocausal item
  // The last consume fails, so the bytestring should be written successfully
  invoke_method(45);

  advance_block();

  // Verify that there's a bytestring
  auto bstring = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(bstring.has_value());
  ASSERT_TRUE(bstring->type() == StorageItemType::kItemTypeBytestring);
}

// Test that we only output the consume if entries are actually consumed.
// Starts from empty, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromEmpty)
{
  // Start from empty, delete (no-op), then insert a new item
  invoke_method(13, uint32_t{42});

  // Then retrocausally consume the item
  invoke_method(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  ASSERT_TRUE(check_rcq_empty());
}

// Test that we only output the consume if entries are actually consumed.
// Starts from empty, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromBytestring)
{
  set_bytestring(make_bytestring("test"));
  advance_block();
  // delete, then insert a new item
  invoke_method(13, uint32_t{42});

  // Then retrocausally consume the item
  invoke_method(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  ASSERT_TRUE(check_rcq_empty());
}

// Test that we only output the consume if entries are actually consumed.
// Starts from empty, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromEmptyQueue)
{
  insert(42);
  invoke_method(1, uint32_t{42});

  advance_block();
  // delete, then insert a new item
  invoke_method(13, uint32_t{42});

  // Then retrocausally consume the item
  invoke_method(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  ASSERT_TRUE(check_rcq_empty());
}
// Test that we only output the consume if entries are actually consumed.
// Starts from nonempty queue, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromNonEmptyQueue)
{
  insert(42);

  advance_block();
  // delete, then insert a new item
  invoke_method(13, uint32_t{99});

  // Consuming the preexisting item should fail
  invoke_method_fail_commit(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);
}

// Peek returns the next entry without advancing the cursor.
// A subsequent consume should return the same entry.
TEST_P(RCQDeleteTest, PeekThenConsumeSameEntry)
{
  insert(42);
  invoke_method(24, uint32_t{42}); // peek verifies value
  invoke_method(1, uint32_t{42});  // consume same entry

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Multiple peeks without intervening consume return the same entry,
// confirming the cursor doesn't advance on peek.
TEST_P(RCQDeleteTest, PeekTwiceReturnsSameEntry)
{
  insert(42);
  insert(99);
  invoke_method(27, uint32_t{42}, uint32_t{42}, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// After delete+recreate, peek should see entries from the new queue.
// Verifies peek state is reset by delete.
TEST_P(RCQDeleteTest, PeekAfterDeleteRecreateSeesNewEntry)
{
  insert(42);
  invoke_method(28, uint32_t{42}, uint32_t{99});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  // peek doesn't consume, so the self-inserted entry (99) is still live
  ASSERT_EQ(qe.live_entries, 1);
}

// After delete, peek can still observe a concurrent entry, but it does not
// recreate the queue or consume the entry. The pending delete still conflicts.
TEST_P(RCQDeleteTest, PeekAfterDeleteSeesConcurrentEntryButDeleteConflicts)
{
  insert(42);
  insert(99);
  invoke_method_fail_commit(33, uint32_t{42}, uint32_t{99});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 2);
}

// A delete inside a reverted try_invoke frame should leave the queue intact.
// Entries remain consumable after the revert.
TEST_P(RCQDeleteTest, RevertedDeletePreservesQueue)
{
  insert(42);
  invoke_method_orchestrated(26, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Same as RevertedDeletePreservesQueue but entries come from a past block.
// Verifies reverting a delete works for committed entries.
TEST_P(RCQDeleteTest, RevertedDeletePreservesQueueFromPastBlock)
{
  insert(42);

  advance_block();

  invoke_method_orchestrated(26, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Insert a self entry, then try_invoke a delete that reverts.
// Both self-inserted and concurrent entries remain consumable.
TEST_P(RCQDeleteTest, RevertedDeleteWithInsertPreservesQueue)
{
  insert(99); // concurrent
  invoke_method_orchestrated(29, uint32_t{42}, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// A TryInvoke that succeeds still has its writes discarded if the frame that
// made it later reverts. The child ran at a third address, so nothing here is
// reentrant: orchestrator -> kContractAddress (reverts) -> kContractAddress3
// (the TryInvoke'd insert).
TEST_P(RCQDeleteTest, RevertedFrameDiscardsSuccessfulNestedTryInvoke)
{
  invoke_method_orchestrated(69, uint32_t{42});

  advance_block();

  // The insert happened in a TryInvoke that succeeded, but its caller
  // reverted, so kContractAddress3's queue must never have received it.
  ASSERT_FALSE(
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress3))
          .has_value())
      << "a reverted frame kept the writes of a successful nested TryInvoke";
}

// A successful delete inside a try_invoke frame should fail to acquire locks
// when a concurrent transaction has inserted into the same queue.
// The delete is reverted due to conflict, and the entry remains consumable.
TEST_P(RCQDeleteTest, TryInvokeDeleteConflictsWithConcurrentInsert)
{
  insert(42); // concurrent insert creates conflicting lock
  invoke_method_orchestrated(32, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// The TryInvoke happy path: an insert in a TryInvoke'd child with nothing to
// conflict with passes the lock probe, commits into the caller, and the
// transaction commits it for real. A second transaction in the same block
// consumes the entry retrocausally, proving the insert landed in the DB
// rather than only in the child frame.
TEST_P(RCQDeleteTest, TryInvokeInsertCommits)
{
  invoke_method_orchestrated(70, uint32_t{0}, uint32_t{42}, uint32_t{0});
  invoke_method(1, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// A consume of a past-block entry inside a successful TryInvoke commits: the
// entry is gone from the queue after the block. This is the flow that pushes
// the item's consume delta twice -- once at the TryInvoke probe and again,
// cumulatively, at root commit.
//
// Skipped on MockDB: the re-push is idempotent on HogDb, but MockDB rejects
// this transaction's second consume push as a competing consumer (see
// mockdb/KNOWN_ISSUES.md).
TEST_P(RCQDeleteTest, TryInvokeConsumeCommits)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB rejects a same-transaction consume re-push (see "
                    "mockdb/KNOWN_ISSUES.md)";
  }

  insert(42);

  advance_block();

  invoke_method_orchestrated(70, uint32_t{1}, uint32_t{42}, uint32_t{0});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Same, but the consumed entry is a concurrent one from another transaction
// in the same block, so the consume is a nondeterministic observation made
// inside the TryInvoke'd frame and then committed through the probe's
// mid-transaction subtransaction push.
TEST_P(RCQDeleteTest, TryInvokeConsumeConcurrentEntryCommits)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB rejects a same-transaction consume re-push (see "
                    "mockdb/KNOWN_ISSUES.md)";
  }

  insert(42); // concurrent
  invoke_method_orchestrated(70, uint32_t{1}, uint32_t{42}, uint32_t{0});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// The insert counterpart of TryInvokeDeleteConflictsWithConcurrentInsert: a
// concurrent transaction's delete holds the queue's exclusive lock, so a
// TryInvoke'd insert fails the probe and comes back Fault instead of
// committing.
TEST_P(RCQDeleteTest, TryInvokeInsertConflictsWithConcurrentDelete)
{
  insert(42);

  advance_block();

  invoke_method(31); // concurrent delete holds the queue's exclusive lock
  invoke_method_orchestrated(70, uint32_t{0}, uint32_t{99}, uint32_t{1});

  advance_block();

  // The delete won; the TryInvoke'd insert left no trace.
  ASSERT_FALSE(lookup_storage_item_from_db(get_queue_addrkey()).has_value())
      << "a conflicted TryInvoke'd insert still reached the queue";
}

// The consume counterpart: the child consumes a past-block entry successfully
// in-frame (against the transaction's snapshot, where the concurrent delete
// is not visible), and the lock probe then conflicts with that delete.
TEST_P(RCQDeleteTest, TryInvokeConsumeConflictsWithConcurrentDelete)
{
  insert(42);

  advance_block();

  invoke_method(31); // concurrent delete holds the queue's exclusive lock
  invoke_method_orchestrated(70, uint32_t{1}, uint32_t{42}, uint32_t{1});

  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(get_queue_addrkey()).has_value())
      << "a conflicted TryInvoke'd consume left queue state behind";
}

// After delete+recreate, entries inserted in the same block should be
// lexicographically sorted when consumed in a later block.
TEST_P(RCQDeleteTest, OrderingPreservedAfterDeleteRecreate)
{
  // Block 1: insert entries
  insert(5);
  insert(1);
  insert(3);

  advance_block();

  // Block 2: delete, insert new entries
  invoke_method(30, uint32_t{4}, uint32_t{2});

  advance_block();

  // Block 3: consume first sorted entry
  invoke_method(1, uint32_t{2});

  advance_block();

  // Block 4: consume second sorted entry
  invoke_method(1, uint32_t{4});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Deleting a queue destroys ALL pending entries across multiple past blocks.
TEST_P(RCQDeleteTest, DeleteDestroysMultiplePastBlocks)
{
  // Block 1: insert(10)
  insert(10);

  advance_block();

  // Block 2: insert(20)
  insert(20);

  advance_block();

  // Block 3: delete, insert(30)
  invoke_method(13, uint32_t{30});

  advance_block();

  // Block 4: co_await consume(30) — 10 and 20 are gone
  invoke_method(1, uint32_t{30});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// A failed inner invoke that inserts an entry should leave no trace — the
// queue should remain empty.
TEST_P(RCQDeleteTest, RevertedInsertDiscardsEntry)
{
  invoke_method_orchestrated(36, uint32_t{42});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  if (!queue.has_value()) {
    return; // queue was never created, trivially passes
  }
}

// After a reverted inner insert, a subsequent successful insert should work
// normally. Only the successful entry is consumable.
TEST_P(RCQDeleteTest, RevertedInsertThenSuccessfulInsert)
{
  invoke_method_orchestrated(37, uint32_t{42}, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// A reverted inner insert does not affect concurrent entries from other
// transactions.
TEST_P(RCQDeleteTest, RevertedInsertPreservesConcurrentEntries)
{
  insert(99); // concurrent
  invoke_method_orchestrated(43, uint32_t{42}, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// A reverted inner insert does not affect entries from past blocks.
TEST_P(RCQDeleteTest, RevertedInsertPreservesPastEntries)
{
  insert(99);

  advance_block();

  invoke_method_orchestrated(43, uint32_t{42}, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// A failed inner invoke that consumes an entry should leave the entry
// available for a subsequent consume.
TEST_P(RCQDeleteTest, RevertedConsumePreservesEntry)
{
  insert(42); // concurrent
  invoke_method_orchestrated(38, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Same as RevertedConsumePreservesEntry but the entry comes from a past block.
TEST_P(RCQDeleteTest, RevertedConsumePreservesPastEntry)
{
  insert(42);

  advance_block();

  invoke_method_orchestrated(38, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// After a reverted inner consume, peek should still see the entry.
TEST_P(RCQDeleteTest, RevertedConsumeThenPeek)
{
  insert(42); // concurrent
  invoke_method_orchestrated(39, uint32_t{42});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1); // peek doesn't consume
}

// An entry survives multiple consecutive reverted inner consume attempts.
TEST_P(RCQDeleteTest, SequentialRevertedConsumes)
{
  insert(42); // concurrent
  invoke_method_orchestrated(40, uint32_t{42});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// A failed inner invoke that both consumes AND inserts should revert both: the
// original entry is available, the inserted entry is gone.
TEST_P(RCQDeleteTest, RevertedConsumeAndInsertPreservesOriginal)
{
  insert(42); // concurrent
  invoke_method_orchestrated(41, uint32_t{42}, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// Insert into an RCQ, commit it to the DAO, then purely delete it in a
// separate block. This exercises DeletionLock::write_committed_change removing
// an RCQ trie entry — the trie type must match kRetrocausalQueue used by
// RetrocausalQueueLock, not some other value.
TEST_P(RCQDeleteTest, PureDeleteOfCommittedRCQ)
{
  // Block 1: insert an entry so the RCQ is written to the DAO
  insert(42);
  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);

  // Block 2: pure delete (method 31) — no other RCQ operations
  invoke_method(31);
  advance_block();

  // Item should be gone from the DAO
  auto after = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_FALSE(after.has_value());
}

// Stores a bytestring containing serialized RetrocausalQueueMetadata with
// inflated live_entries/max_live_block_num. Then deletes it and inserts into an
// RCQ at the same key. Verifies the system doesn't inherit the bogus metadata
// from the bytestring payload -- the RCQ should be treated as fresh.
TEST_P(RCQDeleteTest, BogusMetadataInBytestringIgnored)
{
  // Construct a RetrocausalQueueMetadata with non-zero fields
  RetrocausalQueueMetadata bogus_meta{};
  bogus_meta.live_entries = 9999;
  bogus_meta.max_live_block_num = 42;
  bogus_meta.consume_state.max_fully_consumed_block_number = 10;
  bogus_meta.consume_state.partial_consumed_number = 5;

  // Serialize it and store as a bytestring
  auto buf = x23::serialize(bogus_meta);
  Bytestring bs(buf.begin(), buf.end());
  set_bytestring(bs);
  advance_block();

  // Delete the bytestring, then insert into RCQ at the same key
  invoke_method(13, uint32_t{42});

  // Another tx retrocausally consumes the entry
  invoke_method(1, uint32_t{42});

  advance_block();

  // The queue should be empty: one insert, one consume, net zero.
  // If the bogus metadata leaked through, we'd see 9999+ live entries or
  // a conservation invariant failure.
  ASSERT_TRUE(check_rcq_empty());
}

// Verifies that when an RCQ with entries in past (committed) blocks is
// purely deleted, the underlying block records in the DAO are also removed.
// This catches a storage leak where DeletionLock::write_committed_change only
// removes the metadata record but leaves orphaned block records behind.
TEST_P(RCQDeleteTest, PureDeleteCleansUpPastBlocks)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "Block record check requires HogDb DAO access";
  }

  // Block 1: insert an entry
  insert(10);

  advance_block();

  // Block 2: insert another entry (creates a second past block)
  insert(20);

  advance_block();

  // Sanity: the RCQ exists with 2 entries across 2 blocks.
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 2);

  // Get DAO access to check block records directly.
  auto &dao = get_hog_db_dao();
  auto item_key = make_item_key(get_queue_addrkey());

  // Verify blocks exist before delete.
  auto block1 = dao.read_retrocausal_queue_block(item_key, 1);
  ASSERT_TRUE(block1.has_value()) << "Block 1 should exist before delete";
  auto block2 = dao.read_retrocausal_queue_block(item_key, 2);
  ASSERT_TRUE(block2.has_value()) << "Block 2 should exist before delete";

  // Block 3: pure delete (no insert, no consume)
  invoke_method(31);

  advance_block();

  // Metadata should be gone.
  auto after = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_FALSE(after.has_value()) << "RCQ metadata should be removed";

  // Block records should also be gone.
  auto block1_after = dao.read_retrocausal_queue_block(item_key, 1);
  EXPECT_FALSE(block1_after.has_value())
      << "Block 1 should be removed after pure delete (storage leak)";
  auto block2_after = dao.read_retrocausal_queue_block(item_key, 2);
  EXPECT_FALSE(block2_after.has_value())
      << "Block 2 should be removed after pure delete (storage leak)";
}

// Delete an RCQ with multiple past blocks, then in a later block re-insert.
// The new queue should contain only the newly inserted entry — none of the
// old entries should survive.
TEST_P(RCQDeleteTest, DeleteThenReinsertInLaterBlock)
{
  // Block 1: insert entries that become past block entries
  insert(10);
  insert(20);

  advance_block();

  // Block 2: insert another entry (second past block)
  insert(30);

  advance_block();

  // Sanity: 3 entries across 2 past blocks
  {
    auto queue = lookup_storage_item_from_db(get_queue_addrkey());
    ASSERT_TRUE(queue.has_value());
    ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
    ASSERT_EQ(queue->metadata().live_entries, 3);
  }

  // Block 3: pure delete
  invoke_method(31);

  advance_block();

  // Item should be gone
  {
    auto queue = lookup_storage_item_from_db(get_queue_addrkey());
    ASSERT_FALSE(queue.has_value()) << "RCQ should be removed after delete";
  }

  // Block 4: fresh insert into the same key
  insert(99);

  advance_block();

  // The queue should exist with exactly 1 entry — the newly inserted one.
  {
    auto queue = lookup_storage_item_from_db(get_queue_addrkey());
    ASSERT_TRUE(queue.has_value());
    ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
    ASSERT_EQ(queue->metadata().live_entries, 1);
  }

  // Consume the entry to verify it's the new one (99), not an old one.
  invoke_method(1, uint32_t{99});

  advance_block();

  ASSERT_TRUE(check_rcq_empty());
}

// MultiBlockConsumeRemovesOldBlocks
//
// Verifies that when a consumer drains an RCQ spanning multiple past blocks,
// the consumed block records are actually pruned from RAMDB.  Existing tests
// check metadata (live_entries) but not block-level RAMDB state for pure
// consume (no delete involved).
TEST_P(RCQDeleteTest, MultiBlockConsumeRemovesOldBlocks)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "Block record check requires HogDb DAO access";
  }

  // Block 1: insert 2 entries → creates block 1.
  insert(10);
  insert(20);
  advance_block();

  // Block 2: insert 1 entry → creates block 2.
  insert(30);
  advance_block();

  // Sanity: 3 entries across 2 blocks.
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->metadata().live_entries, 3);

  auto &dao = get_hog_db_dao();
  auto item_key = make_item_key(get_queue_addrkey());

  auto block1 = dao.read_retrocausal_queue_block(item_key, 1);
  ASSERT_TRUE(block1.has_value()) << "Block 1 should exist before consume";
  auto block2 = dao.read_retrocausal_queue_block(item_key, 2);
  ASSERT_TRUE(block2.has_value()) << "Block 2 should exist before consume";

  // Block 3: consume all 3 entries in a single TX.
  invoke_method(50);
  advance_block();

  // Queue should be empty now.
  ASSERT_TRUE(check_rcq_empty());

  // Both old block records must be pruned from RAMDB.
  auto block1_after = dao.read_retrocausal_queue_block(item_key, 1);
  EXPECT_FALSE(block1_after.has_value())
      << "Block 1 should be removed after consuming all entries";
  auto block2_after = dao.read_retrocausal_queue_block(item_key, 2);
  EXPECT_FALSE(block2_after.has_value())
      << "Block 2 should be removed after consuming all entries";
}

// DeleteAcrossBlocksCleansOldBlocks
//
// Verifies that standalone delete (cross-block, not same-TX as recreate)
// cleans up old RCQ block records.  After delete, old blocks are gone.
// After recreate in a later block, the new RCQ creates its own block 1,
// which should not be confused with the old block 1.
TEST_P(RCQDeleteTest, DeleteAcrossBlocksCleansOldBlocks)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "Block record check requires HogDb DAO access";
  }

  // Block 1: insert entries → creates block 1.
  insert(10);
  insert(20);
  advance_block();

  // Block 2: insert more → creates block 2.
  insert(30);
  advance_block();

  auto &dao = get_hog_db_dao();
  auto item_key = make_item_key(get_queue_addrkey());

  ASSERT_TRUE(dao.read_retrocausal_queue_block(item_key, 1).has_value());
  ASSERT_TRUE(dao.read_retrocausal_queue_block(item_key, 2).has_value());

  // Block 3: delete (method 17 = delete + consume_empty).
  invoke_method(17);
  advance_block();

  // Metadata is gone.
  auto after_delete = lookup_storage_item_from_db(get_queue_addrkey());
  EXPECT_FALSE(after_delete.has_value())
      << "Queue metadata should be removed after delete";

  // Old block records are cleaned up.
  EXPECT_FALSE(dao.read_retrocausal_queue_block(item_key, 1).has_value())
      << "Block 1 should be removed after standalone delete";
  EXPECT_FALSE(dao.read_retrocausal_queue_block(item_key, 2).has_value())
      << "Block 2 should be removed after standalone delete";

  // Block 4: recreate with a new entry.
  insert(99);
  advance_block();

  // New queue exists with 1 entry.
  auto after = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(after.has_value());
  ASSERT_EQ(after->metadata().live_entries, 1);

  // New RCQ created block 1 (fresh numbering). Old block 2 is gone.
  EXPECT_TRUE(dao.read_retrocausal_queue_block(item_key, 1).has_value())
      << "New block 1 should exist (fresh RCQ after recreate)";
  EXPECT_FALSE(dao.read_retrocausal_queue_block(item_key, 2).has_value())
      << "No block 2 should exist (old ghost or new)";
}

TEST_P(RCQDeleteTest, ValidationDeleteRecreateAfterConcurrentConsume)
{
  auto make_tx = [this](Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = kContractAddress,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  auto tx_insert = make_tx(0, uint32_t{42});
  auto tx_delete_recreate = make_tx(28, uint32_t{42}, uint32_t{99});

  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData delete_recreate_proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_insert, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    insert_proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler = make_new_system_call_handler(tx_delete_recreate, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    delete_recreate_proposal_data = handler.get_proposal_data();
    ASSERT_EQ(delete_recreate_proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_EQ(
        delete_recreate_proposal_data.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler =
        make_new_system_call_handler_validation(tx_insert, *context_,
                                                insert_proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_delete_recreate, *context_,
                                                delete_recreate_proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationDeleteRecreateMatch)
{
  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  auto tx_insert_a = make_tx(kContractAddress, 0, uint32_t{42});
  auto tx_insert_b = make_tx(kContractAddress2, 0, uint32_t{42});
  auto tx_a = make_tx(kContractAddress, 60, uint32_t{42}, uint32_t{99});
  auto tx_b = make_tx(kContractAddress2, 60, uint32_t{42}, uint32_t{99});

  // Insert concurrent entries on both addresses.
  for (auto const &tx : {tx_insert_a, tx_insert_b}) {
    auto handler = make_new_system_call_handler(tx, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Proposal on address A.
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Validation on address B, reusing A's proposal data.
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  auto queue_a =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress));
  auto queue_b =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress2));
  ASSERT_EQ(queue_a.has_value(), queue_b.has_value());
  if (queue_a.has_value()) {
    EXPECT_EQ(queue_a->type(), queue_b->type());
    EXPECT_EQ(queue_a->metadata().live_entries,
              queue_b->metadata().live_entries);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationConsumeDeleteConsumeMatch)
{
  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  // Insert two concurrent entries on each address.
  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    for (uint32_t v : {uint32_t{42}, uint32_t{99}}) {
      auto tx = make_tx(addr, 0, v);
      auto handler = make_new_system_call_handler(tx, *context_);
      ASSERT_TRUE(handler.root_invoke()().is_root_success());
      ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
    }
  }

  // Method 60: co_await consume(42) -> delete -> insert(77).
  // The second concurrent entry (99) is lost with the delete.
  auto tx_a = make_tx(kContractAddress, 60, uint32_t{42}, uint32_t{77});
  auto tx_b = make_tx(kContractAddress2, 60, uint32_t{42}, uint32_t{77});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  auto queue_a =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress));
  auto queue_b =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress2));
  ASSERT_EQ(queue_a.has_value(), queue_b.has_value());
  if (queue_a.has_value()) {
    EXPECT_EQ(queue_a->type(), queue_b->type());
    EXPECT_EQ(queue_a->metadata().live_entries,
              queue_b->metadata().live_entries);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationMultiConsumeDeleteMatch)
{
  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  // Insert two concurrent entries on each address.
  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    for (uint32_t v : {uint32_t{42}, uint32_t{99}}) {
      auto tx = make_tx(addr, 0, v);
      auto handler = make_new_system_call_handler(tx, *context_);
      ASSERT_TRUE(handler.root_invoke()().is_root_success());
      ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
    }
  }

  // Method 61: co_await consume(42) -> co_await consume(99) -> delete ->
  // insert(77).
  auto tx_a =
      make_tx(kContractAddress, 61, uint32_t{42}, uint32_t{99}, uint32_t{77});
  auto tx_b =
      make_tx(kContractAddress2, 61, uint32_t{42}, uint32_t{99}, uint32_t{77});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  auto queue_a =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress));
  auto queue_b =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress2));
  ASSERT_EQ(queue_a.has_value(), queue_b.has_value());
  if (queue_a.has_value()) {
    EXPECT_EQ(queue_a->type(), queue_b->type());
    EXPECT_EQ(queue_a->metadata().live_entries,
              queue_b->metadata().live_entries);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationMultiCycleDeleteRecreateMatch)
{
  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  // Insert two concurrent entries on each address.
  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    for (uint32_t v : {uint32_t{42}, uint32_t{99}}) {
      auto tx = make_tx(addr, 0, v);
      auto handler = make_new_system_call_handler(tx, *context_);
      ASSERT_TRUE(handler.root_invoke()().is_root_success());
      ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
    }
  }

  // Method 62: co_await
  // consume(42)->delete->insert(55)->consume(55)->delete->insert(77)
  auto tx_a =
      make_tx(kContractAddress, 62, uint32_t{42}, uint32_t{55}, uint32_t{77});
  auto tx_b =
      make_tx(kContractAddress2, 62, uint32_t{42}, uint32_t{55}, uint32_t{77});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  auto queue_a =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress));
  auto queue_b =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress2));
  ASSERT_EQ(queue_a.has_value(), queue_b.has_value());
  if (queue_a.has_value()) {
    EXPECT_EQ(queue_a->type(), queue_b->type());
    EXPECT_EQ(queue_a->metadata().live_entries,
              queue_b->metadata().live_entries);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationPastBlockConsumeDeleteMatch)
{
  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  // Block 1: insert past-block entries on both addresses.
  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    auto tx = make_tx(addr, 0, uint32_t{42});
    auto handler = make_new_system_call_handler(tx, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  // Block 2: insert concurrent entries, then co_await consume(past) -> delete
  // -> insert(new).
  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    auto tx = make_tx(addr, 0, uint32_t{99});
    auto handler = make_new_system_call_handler(tx, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Method 60 consumes a concurrent entry. But here entry 42 is from a past
  // block and will be consumed first (past-block entries are consumed before
  // concurrent entries). So co_await consume(42) -> delete -> insert(77).
  auto tx_a = make_tx(kContractAddress, 60, uint32_t{42}, uint32_t{77});
  auto tx_b = make_tx(kContractAddress2, 60, uint32_t{42}, uint32_t{77});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  auto queue_a =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress));
  auto queue_b =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress2));
  ASSERT_EQ(queue_a.has_value(), queue_b.has_value());
  if (queue_a.has_value()) {
    EXPECT_EQ(queue_a->type(), queue_b->type());
    EXPECT_EQ(queue_a->metadata().live_entries,
              queue_b->metadata().live_entries);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationPeekDeleteConsumeMatch)
{
  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    auto tx = make_tx(addr, 0, uint32_t{42});
    auto handler = make_new_system_call_handler(tx, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Method 63: peek(42) -> delete -> insert(99) -> co_await consume(99) ->
  // co_await consume(42)
  auto tx_a = make_tx(kContractAddress, 63, uint32_t{42}, uint32_t{99});
  auto tx_b = make_tx(kContractAddress2, 63, uint32_t{42}, uint32_t{99});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  auto queue_a =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress));
  auto queue_b =
      lookup_storage_item_from_db(get_queue_addrkey_for(kContractAddress2));
  ASSERT_EQ(queue_a.has_value(), queue_b.has_value());
  if (queue_a.has_value()) {
    EXPECT_EQ(queue_a->type(), queue_b->type());
    EXPECT_EQ(queue_a->metadata().live_entries,
              queue_b->metadata().live_entries);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationPeekOnlyDeleteInsertMatch)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "Block record check requires HogDb DAO access";
  }

  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    auto tx = make_tx(addr, 0, uint32_t{42});
    auto handler = make_new_system_call_handler(tx, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Method 64: peek(42) -> delete -> insert(99). No consume, so
  // retrocausally_consumed_count stays 0 through the delete/recreate.
  auto tx_a = make_tx(kContractAddress, 64, uint32_t{42}, uint32_t{99});
  auto tx_b = make_tx(kContractAddress2, 64, uint32_t{42}, uint32_t{99});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  // peek(42) -> delete -> insert(99): the concurrent entry 42 was not consumed
  // (peek doesn't advance the cursor), plus self-insert 99. Both survive in
  // the recreated queue as block 1 entries.
  auto read_u32 = [](QueueEntry const &e) -> uint32_t {
    uint32_t v = 0;
    std::memcpy(&v, e.data(), sizeof(v));
    return v;
  };

  auto &dao = get_hog_db_dao();
  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    auto queue = lookup_storage_item_from_db(get_queue_addrkey_for(addr));
    ASSERT_TRUE(queue.has_value());
    EXPECT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
    EXPECT_EQ(queue->metadata().live_entries, 2U);

    auto item_key = make_item_key(get_queue_addrkey_for(addr));
    auto block = dao.read_retrocausal_queue_block(item_key, 1);
    ASSERT_TRUE(block.has_value()) << "Block 1 should exist";
    ASSERT_EQ(block->entries.size(), 2U);
    EXPECT_EQ(read_u32(block->entries[0]), 42U);
    EXPECT_EQ(read_u32(block->entries[1]), 99U);
  }
}

TEST_P(RCQDeleteTest, ProposalValidationSelfConsumeDeleteConcurrentConsumeMatch)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "Block record check requires HogDb DAO access";
  }

  auto make_tx = [this](ContractAddress const &addr, Runtime::MethodID method,
                        auto const &...args) -> Transaction {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = x23::serialize(args...)},
                       .uniquifier = make_new_uniquifier()};
  };

  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    auto tx = make_tx(addr, 0, uint32_t{77});
    auto handler = make_new_system_call_handler(tx, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Method 65: insert(10) -> co_await consume(10) -> delete -> insert(20) ->
  //            co_await consume(20) -> co_await consume(77)
  auto tx_a =
      make_tx(kContractAddress, 65, uint32_t{10}, uint32_t{20}, uint32_t{77});
  auto tx_b =
      make_tx(kContractAddress2, 65, uint32_t{10}, uint32_t{20}, uint32_t{77});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_a, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_b, *context_, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  advance_block();

  // All entries consumed: self(10) before delete, self(20) and concurrent(77)
  // after recreate. Queue should not exist (HogDb removes empty queues).
  for (auto const &addr : {kContractAddress, kContractAddress2}) {
    auto queue = lookup_storage_item_from_db(get_queue_addrkey_for(addr));
    EXPECT_FALSE(queue.has_value()) << "Queue should be empty (all consumed)";
  }
}

// The TryInvoke conflict bit under validation mode. The proposal's lock probe
// conflicts with a concurrent insert and logs a false nondeterministic bit;
// validation must consume that bit and fail the child frame the same way,
// without probing any locks itself.
TEST_P(RCQDeleteTest, ProposalValidationTryInvokeDeleteConflictMatch)
{
  // Concurrent insert whose lock the TryInvoke'd delete conflicts with.
  auto tx_insert = make_tx(kContractAddress, 0, uint32_t{42});
  // Method 32: TryInvoke delete (expects the conflict) -> co_await consume(42).
  auto tx_orch = make_tx(kContractAddress2, 32, uint32_t{42});

  NondeterministicProposalData insert_pd;
  NondeterministicProposalData orch_pd;
  {
    auto handler = make_new_system_call_handler(tx_insert, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    insert_pd = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler = make_new_system_call_handler(tx_orch, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    orch_pd = handler.get_proposal_data();
    // Two bits: the TryInvoke lock probe (false, the conflict) and the
    // consume of the concurrent entry, which is also observed.
    ASSERT_EQ(orch_pd.nondeterministic_bit_count, 2U);
    ASSERT_EQ(orch_pd.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler = make_new_system_call_handler_validation(tx_insert, *context_,
                                                           insert_pd);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_orch, *context_, orch_pd);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
}

// A successful TryInvoke under validation mode. The proposal's probe succeeds
// and logs a true bit; validation consumes it, checks the child's queue
// observations against its item views, and bumps the frame's TryInvoke depth
// (note_validation_try_invoke_success) instead of pushing a subtransaction.
//
// Skipped on MockDB for the same reason as TryInvokeConsumeCommits: the
// proposal-phase commit re-pushes the consume delta the probe already pushed.
TEST_P(RCQDeleteTest, ProposalValidationTryInvokeConsumeCommitMatch)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB rejects a same-transaction consume re-push (see "
                    "mockdb/KNOWN_ISSUES.md)";
  }

  auto tx_insert = make_tx(kContractAddress, 0, uint32_t{42});
  // Method 70: TryInvoke co_await consume(42), expecting the frame to commit
  // (Return).
  auto tx_orch =
      make_tx(kContractAddress2, 70, uint32_t{1}, uint32_t{42}, uint32_t{0});

  NondeterministicProposalData insert_pd;
  NondeterministicProposalData orch_pd;
  {
    auto handler = make_new_system_call_handler(tx_insert, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    insert_pd = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler = make_new_system_call_handler(tx_orch, *context_);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    orch_pd = handler.get_proposal_data();
    // Two bits: the consume of the concurrent entry and the TryInvoke lock
    // probe (true, no conflict).
    ASSERT_EQ(orch_pd.nondeterministic_bit_count, 2U);
    ASSERT_EQ(orch_pd.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler = make_new_system_call_handler_validation(tx_insert, *context_,
                                                           insert_pd);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto handler =
        make_new_system_call_handler_validation(tx_orch, *context_, orch_pd);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
}

// Issue #3152 reachability probe (multi-frame). A prior tx commits entry 77,
// which the method-67 tx observes as a concurrent current-block entry. Method
// 67 consumes 77, deletes, then inserts a distinct entry 42 -- with the
// consume and insert each in their own child frame so the delete is flushed as
// its own subtransaction before the recreate. That makes HogDb process the
// recreate as a deletion -> RCQ exclusive upgrade whose current_block_consumed
// (reloaded 77) is not covered by added_entries (42): the exact shape HogDb
// rejects as kInvalidExecutorBehavior -> kRPCUnrecoverable. A correct executor
// must never produce it, so the transaction must not commit unrecoverably. (A
// deterministic per-tx rejection during invoke is a safe outcome and allowed.)
TEST_P(RCQDeleteTest, ConcurrentConsumeDeleteFramePushInsertNotUnrecoverable)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "kInvalidExecutorBehavior guard is HogDb-specific";
  }

  // Committed in its own tx; observable as a concurrent entry to the next tx.
  insert(77);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  // Orchestrated from the second address: method 67 only
                  // invokes, so every RCQ operation still happens in a frame
                  // at kContractAddress and acts on that queue.
                  .invocation =
                      Invocation{.invoked_address = kContractAddress2,
                                 .entrypoint = entrypoint_from_method_id(67),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = x23::serialize(uint32_t{77},
                                                            uint32_t{42})},
                  .uniquifier = make_new_uniquifier(),
              }},
      *context_);

  // The forbidden RCQ delta is pushed when the final child (insert) frame
  // returns -- i.e. during root_invoke, before commit -- so a
  // kInvalidExecutorBehavior surfaces as an Unrecoverable invoke error, not
  // just at commit. Check both. A deterministic per-tx rejection during invoke
  // is a safe outcome and is allowed.
  auto r = handler.root_invoke()();
  if (r.is_root_success()) {
    ASSERT_NE(handler.try_commit_transaction()(),
              ExecResult::ExecResultUnrecoverable)
        << "executor drove HogDb to reject a contract-produced RCQ delta as "
           "invalid executor behavior at commit (issue #3152)";
  }
  else {
    ASSERT_NE(r.result_.error().code(), InvokeErrorCode::Unrecoverable)
        << "executor drove HogDb to reject a contract-produced RCQ delta as "
           "invalid executor behavior during invoke (issue #3152)";
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, RCQDeleteTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
