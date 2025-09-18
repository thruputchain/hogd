#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class RCQTest : public ::testing::TestWithParam<TestDB>,
                public NativeContractTestFramework {
protected:
  RCQTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  void SetUp() override
  {
    if (GetParam() == TestDB::kHogDB) {
      GTEST_SKIP() << "Pending hogdb retrocausal queue";
      return;
    }
    // Setup native contract
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
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
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto const &calldata = handler.get_calldata();
               auto key = make_storage_key("rcq0");

               for (uint8_t val : calldata) {
                 auto res = handler.retrocausal_queue_consume(key, false);
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
                 if (!res.value().has_value()) { // No entry found
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }
                 auto const &entry = res.value().value();
                 if (entry.size() != 1 ||
                     static_cast<uint8_t>(entry[0]) != val) {
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }
               }
               return {};
             }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));

    // mark the contract as existing and deployed within the database
    auto contract_0 = make_native_contract(0);
    write_contract(contract_0);
    write_deployment(contract_address_, contract_0);
  }

  ContractAddress contract_address_{
      make_contract_address(0)}; // Default address
  std::shared_ptr<NativeContext> runtime_context_;
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
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {5, 4, 3, 2, 1}},
                }},
        runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto r = handler.try_commit_transaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(*r);
  }

  // Step 2: Create a transaction that invokes method 1 with calldata [5, 4],
  // and commit that transaction.  The transaction should succeed.
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {5, 4}},
                }},
        runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto r = handler.try_commit_transaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(*r);
  }

  // Step 3: Advance to the next block.
  advance_block();

  // Step 4: Create a transaction that invokes method 0 with calldata [0, 100],
  // and commit that transaction.  The transaction should succeed.
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {0, 100}},
                }},
        runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto r = handler.try_commit_transaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(*r);
  }

  // Step 5: Create a transaction that invokes method 1 with calldata
  // [1, 2, 3, 0, 100], and commit that transaction.  The transaction should
  // succeed.
  //
  // Note: This test is checking additionally that the order of the entries is
  // lexicographically sorted when the block commits.
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_address_,
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {1, 2, 3, 0, 100}},
                }},
        runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto r = handler.try_commit_transaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(*r);
  }
}

INSTANTIATE_TEST_SUITE_P(TestDBs, RCQTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
