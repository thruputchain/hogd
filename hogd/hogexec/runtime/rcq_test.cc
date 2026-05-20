#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
class RCQTest : public ::testing::TestWithParam<TestDB>,
                public NativeContractTestFramework {
protected:
  RCQTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  std::vector<uint8_t> last_consumed_values_;

  std::unique_ptr<RuntimeContext> make_context()
  {
    // Setup native contract
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0, // Insert
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &calldata = ctx.handler().get_calldata();
               auto key = make_storage_key("rcq0");

               for (uint8_t val : calldata) {
                 auto res = ctx.handler().retrocausal_queue_insert(
                     key, std::as_bytes(std::span(&val, 1)));
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
                 if (!res.value().has_value()) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }
               }
               return {};
             }},
            {1, // Consume
             [this](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               // Calldata is the number of items to consume.
               auto const &consume_count = ctx.handler().get_calldata().at(0);
               auto key = make_storage_key("rcq0");

               last_consumed_values_.clear();
               for (uint8_t i = 0; i < consume_count; i++) {
                 auto res = ctx.handler().retrocausal_queue_consume(key, false);
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
                 if (!res.value().has_value()) { // No entry found, break out
                   return {};
                 }
                 auto const &entry = res.value().value();

                 if (entry.size() != 1) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }
                 last_consumed_values_.push_back(entry[0]);
               }

               if (ctx.handler().get_calldata().size() > 1) {
                 uint8_t abort_code = ctx.handler().get_calldata().at(1);
                 if (abort_code == 1) {
                   auto status = ctx.handler().abort_transaction();
                   if (!status.has_value()) {
                     return status.error().to_invoke_error();
                   }
                   return InvokeError{InvokeErrorCode::Abandon};
                 }
               }

               return {};
             }},
            {2, // Insert and Consume
             [this](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &calldata = ctx.handler().get_calldata();
               // Format: [count_to_consume, val1, val2, ...]
               // First insert all values, then consume 'count_to_consume'
               // items.
               uint8_t consume_count = calldata.at(0);
               auto key = make_storage_key("rcq0");

               // Insert all values (bytes after the first)
               for (size_t i = 1; i < calldata.size(); i++) {
                 auto res = ctx.handler().retrocausal_queue_insert(
                     key, std::as_bytes(std::span(&calldata[i], 1)));
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
                 if (!res.value().has_value()) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }
               }

               // Then consume
               last_consumed_values_.clear();
               for (uint8_t i = 0; i < consume_count; i++) {
                 auto res = ctx.handler().retrocausal_queue_consume(key, false);
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
                 if (!res.value().has_value()) {
                   return {};
                 }
                 auto const &entry = res.value().value();
                 if (entry.size() != 1) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }
                 last_consumed_values_.push_back(entry[0]);
               }
               return {};
             }},
            {3, // Write NNInt contract storage address
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &value = ctx.handler().get_calldata().at(0);
               auto key = make_storage_key("nnint0");

               auto r = ctx.handler().set_nnint(key, {value}, 0, {});
               if (!r.has_value()) {
                 return r.error().to_invoke_error();
               }

               return {};
             }},
            {4, // Insert entire calldata as a single RCQ entry
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &calldata = ctx.handler().get_calldata();
               auto key = make_storage_key("rcq0");
               auto res = ctx.handler().retrocausal_queue_insert(
                   key, std::as_bytes(std::span(calldata)));
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               return {};
             }},
            {5, // Set bytestring on rcq0 key (for type-change testing)
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &calldata = ctx.handler().get_calldata();
               auto key = make_storage_key("rcq0");
               auto res = ctx.handler().set_bytestring(
                   key, std::as_bytes(std::span(calldata)),
                   std::to_underlying(StorageDeltaFlags::None));
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               return {};
             }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  void SetUp() override
  {
    // mark the contract as existing and deployed within the database
    auto contract_0 = make_native_contract(0);
    write_contract(contract_0);
    write_deployment(contract_address_, contract_0);
    context_ = make_context();
  }

  std::unique_ptr<RuntimeContext> context_;

  ContractAddress contract_address_{
      make_contract_address(0)}; // Default address

  testing::AssertionResult invoke_contract(int32_t method_id,
                                           std::vector<uint8_t> const &calldata)
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint =
                                       entrypoint_from_method_id(method_id),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = Calldata(calldata.begin(),
                                                        calldata.end())},
                }},
        *context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return testing::AssertionFailure() << "Root invoke failed";
    }

    auto r = handler.try_commit_transaction();
    if (r != ExecResult::ExecResultOK) {
      return testing::AssertionFailure() << "Commit failed: " << x23::show(r);
    }

    return testing::AssertionSuccess();
  }

  testing::AssertionResult set_nnint(uint8_t value)
  {
    SCOPED_TRACE(
        std::format("set_nnint: value {}", testing::PrintToString(value)));
    return invoke_contract(3, {value});
  }

  // Insert values into the rcq.
  testing::AssertionResult insert(std::vector<uint8_t> const &values)
  {
    SCOPED_TRACE(
        std::format("insert: values {}", testing::PrintToString(values)));
    return invoke_contract(0, values);
  }

  // Consume values from the rcq and ensure they match the expected values.
  testing::AssertionResult consume(std::vector<uint8_t> const &expected_values,
                                   bool abort = false)
  {
    if (expected_values.size() > std::numeric_limits<uint8_t>::max()) {
      return testing::AssertionFailure() << "Too many values to consume";
    }

    SCOPED_TRACE(std::format("consume: expected_values {}",
                             testing::PrintToString(expected_values)));
    auto args =
        std::vector<uint8_t>{static_cast<uint8_t>(expected_values.size()),
                             static_cast<uint8_t>(abort)};
    auto r = invoke_contract(1, args);
    if (!r) {
      return r;
    }

    if (last_consumed_values_ == expected_values) {
      return testing::AssertionSuccess();
    }

    return testing::AssertionFailure()
           << "Value mismatch!"
           << "\n  Actual:   " << testing::PrintToString(last_consumed_values_)
           << "\n  Expected: " << testing::PrintToString(expected_values);
  }

  // Insert values and then immediately consume 'consume_count' items in the
  // same transaction.
  testing::AssertionResult
  insert_and_consume(std::vector<uint8_t> const &values,
                     std::vector<uint8_t> const &expected_consumed)
  {
    std::vector<uint8_t> calldata;
    calldata.push_back(expected_consumed.size());
    calldata.insert(calldata.end(), values.begin(), values.end());

    SCOPED_TRACE(std::format(
        "insert_and_consume: values {} consume_count {} expected {}",
        testing::PrintToString(values), expected_consumed.size(),
        testing::PrintToString(expected_consumed)));

    auto r = invoke_contract(2, calldata);
    if (!r) {
      return r;
    }

    if (last_consumed_values_ == expected_consumed) {
      return testing::AssertionSuccess();
    }
    return testing::AssertionFailure()
           << "Value mismatch!"
           << "\n  Actual:   " << testing::PrintToString(last_consumed_values_)
           << "\n  Expected: " << testing::PrintToString(expected_consumed);
  }

  testing::AssertionResult check_balance(int128_t expected_balance)
  {
    auto key = make_free_native_token_balance_addrkey(contract_address_);
    auto item_opt = lookup_storage_item_from_db(key);
    if (item_opt) {
      int128_t balance = from_xdr(item_opt->nnint());
      return balance == expected_balance
                 ? testing::AssertionSuccess()
                 : testing::AssertionFailure()
                       << "Balance mismatch!"
                       << "\n  Actual:   " << testing::PrintToString(balance)
                       << "\n  Expected: "
                       << testing::PrintToString(expected_balance)
                       << "\n  Difference From Expected: "
                       << testing::PrintToString(expected_balance - balance);
    }
    return testing::AssertionFailure() << "Balance not found!";
  }
};

TEST_P(RCQTest, InsertThenConsume)
{
  auto const initial_balance = 1'000'000'000;
  setup_grant_storage_token(contract_address_, initial_balance);

  ASSERT_TRUE(insert({1, 2, 3}));

  ASSERT_TRUE(consume({1, 2}));

  advance_block();

  // The RCQ metadata item and one RCQ block with one RCQ entry remain.
  ASSERT_TRUE(
      check_balance(initial_balance -
                    get_default_cost_model()->get_retrocausal_queue_cost(1)));

  ASSERT_TRUE(consume({3}));

  // The queue is now empty, so the storage cost should be zero.
  advance_block();

  ASSERT_TRUE(check_balance(initial_balance));
}

TEST_P(RCQTest, InsertThenAbortOnConsume)
{
  // Abort Case - Insert items, then attempt a consume that fails.
  // Verify that lock state is reverted as if the consume operation never
  // happened, and the initial insert's reservation is preserved.

  auto const initial_balance = 1'000'000'000;
  setup_grant_storage_token(contract_address_, initial_balance);

  ASSERT_TRUE(insert({1, 2, 3}));

  // Consume 1 item, but then intentionally abort.
  ASSERT_FALSE(consume({1}, true));

  // Reaching into the locks to verify the state is tricky. Instead infer the
  // state by trying new consumes.
  ASSERT_TRUE(consume({1, 2}));

  advance_block();

  // The RCQ metadata item and one RCQ block with one RCQ entry remain.
  ASSERT_TRUE(
      check_balance(initial_balance -
                    get_default_cost_model()->get_retrocausal_queue_cost(1)));

  ASSERT_TRUE(consume({3}));

  // The queue is now empty, so the cost should be zero.
  advance_block();

  ASSERT_TRUE(check_balance(initial_balance));
}

TEST_P(RCQTest, ConcurrentConsumerAndInserterRaceFix)
{
  // This test targets a race where an inserter TX could overwrite the
  // consumed_past_entries_ of a consumer TX.
  setup_grant_storage_token(contract_address_, 1'000'000'000);
  ASSERT_TRUE(insert({1, 2, 3}));
  advance_block();

  // T1: Consumer TX. Propose consume.
  auto handler_t1 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = contract_address_,
                                    .entrypoint =
                                        entrypoint_from_method_id(1), // Consume
                                    .gas_limit = UINT64_MAX,
                                    .calldata = Calldata({1}),
                                }}}, // Consume 1 item
      *context_);
  ASSERT_TRUE(handler_t1.root_invoke().is_root_success());
  // At this point, T1's RetrocausalQueueIt should have consumed_past_entries_
  // = 1.

  // T2: Inserter TX. Propose insert.
  auto handler_t2 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_address_,
                                           .entrypoint =
                                               entrypoint_from_method_id(
                                                   0), // Insert
                                           .gas_limit = UINT64_MAX,
                                           .calldata = Calldata({5, 4})}}},
      *context_);
  ASSERT_TRUE(handler_t2.root_invoke().is_root_success());
  // T2's RetrocausalQueueIt should have consumed_past_entries_ = 0.
  // In the OLD buggy code, this would have overwritten the global lock's
  // consumed_past_entries_ to 0.

  // T1 Commits.
  ASSERT_EQ(handler_t1.try_commit_transaction(), ExecResult::ExecResultOK);

  ASSERT_FALSE(
      consume({2, 3})); // Aborts because of T1 consume (single consumer).

  // T2 Commits.
  ASSERT_EQ(handler_t2.try_commit_transaction(), ExecResult::ExecResultOK);
  advance_block();

  // Verify final state: {2, 3, 4, 5} exactly.
  ASSERT_TRUE(consume({2, 3}));
  advance_block();
  ASSERT_TRUE(consume({4, 5}));
  advance_block();
  ASSERT_FALSE(consume({1})); // Queue should be empty.
}

// Syscall-level mirror of the direct RCQ assert repro sequence:
// 1) create two committed past blocks, each with one entry
// 2) in a later block, run one tx that consumes both entries
// 3) concurrently run one tx that inserts a new entry
// 4) commit consume first, then insert
TEST_P(RCQTest, SyscallReproInvokeTxLockedStorageAssertOnConcurrentRCQ)
{
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  ASSERT_TRUE(insert({1}));
  advance_block();
  ASSERT_TRUE(insert({2}));
  advance_block();

  auto handler_consume = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_address_,
                                           .entrypoint =
                                               entrypoint_from_method_id(
                                                   1), // Consume
                                           .gas_limit = UINT64_MAX,
                                           .calldata = Calldata({2})}}},
      *context_);
  ASSERT_TRUE(handler_consume.root_invoke().is_root_success());

  auto handler_insert = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_address_,
                                           .entrypoint =
                                               entrypoint_from_method_id(
                                                   0), // Insert
                                           .gas_limit = UINT64_MAX,
                                           .calldata = Calldata({9})}}},
      *context_);
  ASSERT_TRUE(handler_insert.root_invoke().is_root_success());

  // If the invariant is violated, one of these commits will abort the process.
  ASSERT_EQ(handler_consume.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_EQ(handler_insert.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_P(RCQTest, ConsumeFromPastBlock)
{
  // Creates a native contract with the following methods:
  // 0: Loads the calldata and interprets it as a sequence of uint8_t values.
  //    For each of those values, it inserts a new entry into a retrocausal
  //    queue at storage key "rcq0".
  // 1: Loads the calldata and interprets it as a sequence of uint8_t values.
  //    For each of those values, it attempts to consume an entry from the queue
  //    at storage key "rcq0", and checks the returned entry against the value.
  //    If an entry is not present, it returns an error.

  // Test that an rcq consumes from past block:

  // Step 0: grant storage tokens to the contract
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  // Step 1: Create a transaction that invokes method 0 with calldata [5, 4, 3,
  // 2, 1], and commit that transaction.  The transaction should succeed.
  ASSERT_TRUE(insert({5, 4, 3, 2, 1}));

  // Step 2: Create a transaction that invokes method 1 with calldata [5, 4],
  // and commit that transaction.  The transaction should succeed.
  // This consumes 5 and 4 from the rcq.
  // This tests the case that rcq can consume entries inserted in the current
  // block.
  // NOTE: {5, 4} isn't *guaranteed* by the RCQ spec. Since it is concurrent
  // with the insert, it's possible that the rcq will consume a different set
  // of values.  However, the current implementation ensures it when run
  // in this test.
  ASSERT_TRUE(consume({5, 4}));

  // Step 3: Advance to the next block.
  advance_block();

  // Step 4: Create a transaction that invokes method 0 with calldata [0, 100],
  // and commit that transaction.  The transaction should succeed.
  ASSERT_TRUE(insert({0, 100}));

  // Step 5: Create a transaction that invokes method 1 with calldata
  // [1, 2, 3, 0, 100], and commit that transaction.  The transaction should
  // succeed.
  //
  // Note: This test is checking additionally that the order of the entries is
  // lexicographically sorted when the block commits.
  // This tests the case that rcq can consume entries inserted in earlier
  // blocks.
  ASSERT_TRUE(consume({1, 2, 3, 0, 100}));
}

TEST_P(RCQTest, ConsumeNoConcurrentInserts)
{
  // See prior test. The only difference is that this test never tries to
  // consume entries that were inserted in the current block.
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  ASSERT_TRUE(insert({5, 4, 3, 2, 1}));
  advance_block();
  ASSERT_TRUE(consume({1, 2}));
  advance_block();
  ASSERT_TRUE(insert({0, 100}));
  advance_block();
  ASSERT_TRUE(consume({3, 4, 5, 0, 100}));
}

TEST_P(RCQTest, InvariantStrictSingleConsumerPerBlock)
{
  // Invariant 1: Strict Single-Consumer Per Block
  // The RCQ enforces a "Multiple Producer, Single Consumer" (MPSC) pattern
  // within a block. If two independent transactions attempt to consume from the
  // same RCQ in the same block, one must fail with HogDBCode::kConflict.

  setup_grant_storage_token(contract_address_, 1'000'000'000);
  ASSERT_TRUE(insert({1, 2, 3}));
  advance_block();
  ASSERT_TRUE(consume({1}));
  // Second consume in the same block strictly forbidden (simulates concurrent
  // access) This verifies that we lock the queue for consumption for the entire
  // block duration.
  ASSERT_FALSE(consume({2}));
}

TEST_P(RCQTest, InvariantInterBlockSortedOrdering)
{
  // Invariant 3: Inter-Block Sorted Ordering
  // Items that are committed (persist to storage) are reordered.
  // When consuming items from *previous* blocks, they are processed in
  // ascending lexicographical order (per block), regardless of their original
  // insertion order during `insert`.

  setup_grant_storage_token(contract_address_, 1'000'000'000);
  ASSERT_TRUE(insert({5, 1, 4}));
  advance_block();
  // Now consume. Since they are from a past block, they should be sorted.
  ASSERT_TRUE(consume({1, 4, 5}));
}

TEST_P(RCQTest, InvariantConsumptionPersistence)
{
  // Invariant 4: Consumption Persistence (No Zombies)
  // Items successfully consumed in the current block must be permanently
  // removed and never reappear in subsequent blocks.

  setup_grant_storage_token(contract_address_, 1'000'000'000);
  ASSERT_TRUE(insert({1, 2, 3}));
  advance_block();
  ASSERT_TRUE(consume({1}));
  advance_block();
  // Verify '1' is gone forever. If we consume 2 items, we should get 2, 3.
  ASSERT_TRUE(consume({2, 3}));
}

TEST_P(RCQTest, InvariantBlockPruningAndCost)
{
  // Invariant 5: Block Pruning & Cost Accuracy
  // When all items in a committed block are fully consumed, the underlying
  // storage block should be reclaimed, and the storage cost charged to the
  // contract should decrease accordingly.

  setup_grant_storage_token(contract_address_, 1'000'000'000);
  ASSERT_TRUE(insert({10, 11, 12}));
  advance_block();
  // Consume everything.
  // Note: To fully verify this invariant, one should check the storage cost
  // charged to the contract is lower after this block is finalized.
  ASSERT_TRUE(consume({10, 11, 12}));
}

TEST_P(RCQTest, MixedConcurrentTest)
{
  // Checks to ensure self dequeues are prioritized over concurrent dequeues and
  // that persistence works correctly.
  setup_grant_storage_token(contract_address_, 1'000'000'000);
  ASSERT_TRUE(insert({10}));
  ASSERT_TRUE(insert_and_consume({20}, {20, 10}));
  advance_block();
  ASSERT_TRUE(consume({}));
}

TEST_P(RCQTest, ConcurrentPartialConsume)
{
  setup_grant_storage_token(contract_address_, 1'000'000'000);
  ASSERT_TRUE(insert({10}));
  // Expect {20} (Self) because Self > Concurrent in consumption order.
  ASSERT_TRUE(insert_and_consume({20}, {20}));
  advance_block();
  ASSERT_TRUE(consume({10}));
}

TEST_P(RCQTest, ConsumeReturnsStorageCost)
{
  const int128_t hogs = 1'000'000'000;
  setup_grant_storage_token(contract_address_, hogs);

  ASSERT_TRUE(insert({1}));
  advance_block();

  ASSERT_TRUE(consume({1}));
  advance_block();
  ASSERT_TRUE(check_balance(hogs));
}

TEST_P(RCQTest, OverflowBlockImmediately)
{
  if (GetParam() == TestDB::kMockInMemory) {
    // MockDB does not enforce block size limits
    SUCCEED();
    return;
  }
  setup_grant_storage_token(contract_address_, 1'000'000'000);
  std::vector<uint8_t> values(kRetrocausalQueueBlockEntriesMaxLen + 1, 1);
  ASSERT_FALSE(insert(values));
}

TEST_P(RCQTest, OverflowBlockSequentially)
{
  if (GetParam() == TestDB::kMockInMemory) {
    // MockDB does not enforce block size limits
    SUCCEED();
    return;
  }
  setup_grant_storage_token(contract_address_, 1'000'000'000);
  // Insert up to the limit is fine.
  std::vector<uint8_t> values_chunk1(kRetrocausalQueueBlockEntriesMaxLen - 10,
                                     1);
  ASSERT_TRUE(insert(values_chunk1));

  // Inserting more that exceeds the limit should fail.
  std::vector<uint8_t> values_chunk2(11, 2);
  // Total = (Max - 10) + 11 = Max + 1. Should fail.
  ASSERT_FALSE(insert(values_chunk2));
}

TEST_P(RCQTest, ConcurrentOverflow)
{
  if (GetParam() == TestDB::kMockInMemory) {
    // MockDB does not enforce block size limits
    SUCCEED();
    return;
  }
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  // Both T1 and T2 try to insert 600 items. Limit is 1000.
  // Run them concurrently.
  std::vector<uint8_t> values(600, 1);

  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = contract_address_,
                          .entrypoint = entrypoint_from_method_id(0), // Insert
                          .calldata = Calldata(values.begin(), values.end())}}},
      *context_);

  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = contract_address_,
                          .entrypoint = entrypoint_from_method_id(0), // Insert
                          .calldata = Calldata(values.begin(), values.end())}}},
      *context_);

  ASSERT_TRUE(handler1.root_invoke().is_root_success());
  ASSERT_TRUE(handler2.root_invoke().is_root_success());

  // First commit should succeed.
  ASSERT_EQ(handler1.try_commit_transaction(), ExecResult::ExecResultOK);

  // Second commit should fail because it exceeds the block limit.
  ASSERT_EQ(handler2.try_commit_transaction(),
            ExecResult::ExecResultTxNonDeterministicError);
}

// --- Batch 4: Cost model edge cases ---

// CostAccountingMultiBlockPartialConsume
//
// Verifies storage cost accounting across multiple blocks with partial
// consumption.  Inserts entries across 2 blocks, partially consumes from
// past block, and checks that the balance reflects the correct live_entries
// count after each step.
TEST_P(RCQTest, CostAccountingMultiBlockPartialConsume)
{
  auto const hogs = 1'000'000'000;
  setup_grant_storage_token(contract_address_, hogs);
  auto const *cm = get_default_cost_model();

  // Block 1: insert 3 entries.
  ASSERT_TRUE(insert({1, 2, 3}));
  advance_block();

  // 3 live entries → cost = get_retrocausal_queue_cost(3).
  ASSERT_TRUE(check_balance(hogs - cm->get_retrocausal_queue_cost(3)));

  // Block 2: insert 2 more entries (5 total).
  ASSERT_TRUE(insert({4, 5}));
  advance_block();

  ASSERT_TRUE(check_balance(hogs - cm->get_retrocausal_queue_cost(5)));

  // Block 3: consume 2 entries from past block 1 (entries 1, 2).
  // 3 live entries remain.
  ASSERT_TRUE(consume({1, 2}));
  advance_block();

  ASSERT_TRUE(check_balance(hogs - cm->get_retrocausal_queue_cost(3)));

  // Block 4: consume remaining 3 (entry 3 from block 1, 4 and 5 from block 2).
  ASSERT_TRUE(consume({3, 4, 5}));
  advance_block();

  // Queue empty → full refund.
  ASSERT_TRUE(check_balance(hogs));
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// --- Batch 5: Boundary conditions ---

// MaxEntrySizeBoundary
//
// Tests the kQueueEntryMaxLen (256 byte) boundary.  A single entry of exactly
// 256 bytes should succeed; 257 bytes should fail at the syscall layer
// with kSyscallErrorCodeInvalidSyscallInput.
TEST_P(RCQTest, MaxEntrySizeBoundary)
{
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  // 256 bytes: exactly at the limit — should succeed.
  // Use method 4 which inserts the entire calldata as one entry.
  std::vector<uint8_t> at_limit(256, 0xAA);
  ASSERT_TRUE(invoke_contract(4, at_limit));

  advance_block();

  // Verify the entry was stored.
  auto queue = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_address_, "rcq0"));
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->metadata().live_entries, 1);

  // 257 bytes: one past the limit — should fail.
  std::vector<uint8_t> over_limit(257, 0xBB);
  ASSERT_FALSE(invoke_contract(4, over_limit));
}

// ExactBlockEntriesLimit
//
// Inserts exactly kRetrocausalQueueBlockEntriesMaxLen entries (1000)
// in a single TX.  Should succeed without overflow.
TEST_P(RCQTest, ExactBlockEntriesLimit)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "Requires HogDB for block-level verification";
  }
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  std::vector<uint8_t> values(kRetrocausalQueueBlockEntriesMaxLen, 1);
  ASSERT_TRUE(insert(values));

  advance_block();

  auto queue = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_address_, "rcq0"));
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->metadata().live_entries,
            kRetrocausalQueueBlockEntriesMaxLen);
}

// ConcurrentFillToExactLimit
//
// Two TXs each insert 500 entries (total = 1000 =
// kRetrocausalQueueBlockEntriesMaxLen). Both should commit successfully since
// they fill exactly to the limit.
TEST_P(RCQTest, ConcurrentFillToExactLimit)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "Requires HogDB for block-level verification";
  }
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  std::vector<uint8_t> values(kRetrocausalQueueBlockEntriesMaxLen / 2, 1);

  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_address_,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata =
                                     Calldata(values.begin(), values.end())}}},
      *context_);

  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_address_,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata =
                                     Calldata(values.begin(), values.end())}}},
      *context_);

  ASSERT_TRUE(handler1.root_invoke().is_root_success());
  ASSERT_TRUE(handler2.root_invoke().is_root_success());

  // Both should commit: 500 + 500 = 1000 = exactly at limit.
  ASSERT_EQ(handler1.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_EQ(handler2.try_commit_transaction(), ExecResult::ExecResultOK);

  advance_block();

  // Verify the queue actually contains all 1000 entries (not just 500).
  auto queue = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_address_, "rcq0"));
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->metadata().live_entries,
            kRetrocausalQueueBlockEntriesMaxLen);
}

// Fuzz-found bug: cross-block type change (bytestring -> RCQ) followed by
// a second RCQ insert TX in the same block triggers CHECK failure in
// InvokeTX::commit() at invoke_tx.cc:191:
//   cost_change <= vec.back().locked_storage_cost [32 < 22]
//
// Sequence: Block 1 sets a bytestring on key "rcq0".  Block 2 has two
// transactions: TX1 does RCQ insert (type change bytestring -> RCQ), TX2
// does another RCQ insert on the same key.  TX2's commit fails because
// the cost accounting for the second transaction in the same block doesn't
// correctly account for the type change done by TX1.
//
TEST_P(RCQTest, CrossBlockTypeChangeBytestringToRCQCostTracking)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "Requires HogDB for cost tracking";
  }

  auto context = make_context();
  // The fuzz crash starts with 1B tokens granted once at init. After
  // many blocks of operations, the balance is partially consumed. Grant
  // a large amount — the bug is in cost *tracking*, not insufficient funds.
  setup_grant_storage_token(contract_address_, 1'000'000'000);

  // Block 1: set bytestring on "rcq0" key.
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint = entrypoint_from_method_id(5),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = Calldata({0x2f})}}},
        *context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: Two separate transactions, both RCQ insert on "rcq0".
  // TX1 does the type change (bytestring -> RCQ).  TX2 inserts more entries
  // into the now-RCQ key.  The crash happens on TX2's commit: TX1 already
  // acquired the lock for this key in this block, so TX2's lock() returns
  // 0 marginal cost, leaving locked_storage_cost = 0, but commit_change
  // returns the per-entry RCQ cost (8).
  {
    auto handler1 = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = Calldata({0x41})}}},
        *context);
    ASSERT_TRUE(handler1.root_invoke().is_root_success());
    ASSERT_EQ(handler1.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  {
    auto handler2 = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata =
                                       Calldata({0x07, 0x02, 0x42, 0x43})}}},
        *context);
    ASSERT_TRUE(handler2.root_invoke().is_root_success());
    // Before the fix (#1357), this triggered:
    //   CHECK_LE(cost_change, locked_storage_cost) [8 < 0]
    // TX2's reservation was 0 because snapshot_cost_transform clamped
    // against the old bytestring cost, but commit_change returned 8.
    // Now fixed: try_atomic_change_lock returns a conservative standalone
    // cost that survives the snapshot_cost_transform subtraction.
    ASSERT_EQ(handler2.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, RCQTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
