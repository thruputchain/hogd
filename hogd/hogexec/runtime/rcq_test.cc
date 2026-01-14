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
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"

namespace hogd {

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
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto const &calldata = handler.get_calldata();
               auto key = make_storage_key("rcq0");

               for (uint8_t val : calldata) {
                 auto res = handler.retrocausal_queue_insert(
                     key, std::as_bytes(std::span(&val, 1)));
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
               }
               return {};
             }},
            {1,
             [this](SystemCallHandler &handler,
                    NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto const &consume_count = handler.get_calldata().at(0);
               auto key = make_storage_key("rcq0");

               last_consumed_values_.clear();
               for (uint8_t i = 0; i < consume_count; i++) {
                 auto res = handler.retrocausal_queue_consume(key, false);
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
                 if (!res.value().has_value()) { // No entry found, break out
                   return {};
                 }
                 auto const &entry = res.value().value();

                 if (entry.size() != 1) {
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }
                 last_consumed_values_.push_back(entry[0]);
               }
               return {};
             }},
            {2,
             [this](SystemCallHandler &handler,
                    NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto const &calldata = handler.get_calldata();
               // Format: [count_to_consume, val1, val2, ...]
               // First insert all values, then consume 'count_to_consume'
               // items.
               uint8_t consume_count = calldata.at(0);
               auto key = make_storage_key("rcq0");

               for (size_t i = 1; i < calldata.size(); i++) {
                 uint8_t val = calldata[i];
                 auto res = handler.retrocausal_queue_insert(
                     key, std::as_bytes(std::span(&val, 1)));
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
               }

               last_consumed_values_.clear();
               for (uint8_t i = 0; i < consume_count; i++) {
                 auto res = handler.retrocausal_queue_consume(key, false);
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
                 if (!res.value().has_value()) {
                   return {};
                 }
                 auto const &entry = res.value().value();
                 last_consumed_values_.push_back(entry[0]);
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

  // Insert values into the rcq.
  testing::AssertionResult insert(std::vector<uint8_t> const &values)
  {
    SCOPED_TRACE(std::format("insert: values {}", values));
    return invoke_contract(0, values);
  }

  // Consume values from the rcq and ensure they match the expected values.
  testing::AssertionResult consume(std::vector<uint8_t> const &expected_values)
  {
    if (expected_values.size() > std::numeric_limits<uint8_t>::max()) {
      return testing::AssertionFailure() << "Too many values to consume";
    }

    SCOPED_TRACE(std::format("consume: expected_values {}", expected_values));
    auto r = invoke_contract(1, {static_cast<uint8_t>(expected_values.size())});
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
        "insert_and_consume: values {} consume_count {} expected {}", values,
        expected_consumed.size(), expected_consumed));

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
                       << "\n  Actual:   " << std::format("{}", balance)
                       << "\n  Expected: "
                       << std::format("{}", expected_balance)
                       << "\n  Difference From Expected: "
                       << std::format("{}", expected_balance - balance);
    }
    return testing::AssertionFailure() << "Balance not found!";
  }
};

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

TEST_P(RCQTest, DISABLED_ConsumeReturnsStorageCost)
{
  // TODO(stutsman): This should hold, but I think it fails because empty queues
  // don't currently clean up their metadata block.

  const int128_t hogs = 1'000'000'000;
  setup_grant_storage_token(contract_address_, hogs);

  ASSERT_TRUE(insert({1}));
  advance_block();

  ASSERT_TRUE(consume({1}));
  advance_block();
  ASSERT_TRUE(check_balance(hogs));
}

INSTANTIATE_TEST_SUITE_P(TestDBs, RCQTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
