#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <set>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "built_contracts/fibonacci_call.h"
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_block.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class GenesisBlockUnfurlTest : public ::testing::TestWithParam<TestDB>,
                               public NativeContractTestFramework {
public:
  GenesisBlockUnfurlTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam(),
          false)
  {}

protected:
  void SetUp() override { wasm_runtime_context_ = make_hogexec_context(); }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;

  void check_uniquifier(const Uniquifier &uniquifier)
  {
    auto r = uniquifiers_.insert(uniquifier);
    EXPECT_TRUE(r.second);
  }

  static SHA256Hash make_nonce(uint64_t nonce)
  {
    SHA256Hash nonce_hash{};
    std::memcpy(nonce_hash.data(), &nonce, sizeof(nonce));
    return nonce_hash;
  }

  std::set<Uniquifier> uniquifiers_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_P(GenesisBlockUnfurlTest, LaunchFibonacciContractAndCaller)
{
  auto fibonacci_contract = load_contract("fibonacci.wasm");
  auto caller_contract = load_contract("fibonacci_call.wasm");

  auto fibonacci_nonce = make_nonce(0);
  auto caller_nonce = make_nonce(1);

  auto expected_fibonacci_address =
      GenesisBlockLoader::calculate_deployed_address(sha256(
                                                         *fibonacci_contract),
                                                     fibonacci_nonce);
  auto expected_caller_address =
      GenesisBlockLoader::calculate_deployed_address(sha256(*caller_contract),
                                                     caller_nonce);

  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;
  using caller_meta_t = ContractTraitMeta<fibonacci_call::FibonacciCaller>;

  GenesisBlockBuilder builder;
  builder.add_initial_config(fibonacci_contract, fibonacci_nonce,
                             fibonacci_meta_t::entrypoint<"fibonacci">(),
                             fibonacci_meta_t::make_calldata<"fibonacci">(
                                 uint32_t{0}),
                             int128_t{1'000'000'000});
  builder.add_initial_config(caller_contract, caller_nonce,
                             caller_meta_t::entrypoint<"initialize">(),
                             caller_meta_t::make_calldata<"initialize">(
                                 expected_fibonacci_address),
                             int128_t{1'000'000'000});
  auto block = builder.build();

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  EXPECT_EQ(unfurl_txns.size(), 2);

  run_genesis_initialization(block);

  // run the unfurl transactions

  for (auto const &tx : unfurl_txns) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx, wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  advance_block();

  // Should be able to invoke the fibonacci caller contract now
  auto caller_client = ContractTraitClient<fibonacci_call::FibonacciCaller>(
      expected_caller_address);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation =
                                caller_client.make_invocation<"call_fibonacci">(
                                    UINT64_MAX, uint32_t{5}, UINT64_MAX)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  // check the storage
  {
    auto token_addrkey =
        make_free_native_token_balance_addrkey(expected_caller_address);
    auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
    ASSERT_TRUE(token_item_opt.has_value());
    ASSERT_EQ(token_item_opt.value().type(),
              StorageItemType::kItemTypeNonnegativeInt);
    if (GetParam() == TestDB::kMockInMemory) {
      EXPECT_LE(from_xdr(token_item_opt.value().nnint()), 1'000'000'000);
    }
    else {
      warn("Hogdb storage cost accounting is backwards");
    }
  }

  {
    auto token_addrkey =
        make_free_native_token_balance_addrkey(expected_fibonacci_address);
    auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
    ASSERT_TRUE(token_item_opt.has_value());
    ASSERT_EQ(token_item_opt.value().type(),
              StorageItemType::kItemTypeNonnegativeInt);
    if (GetParam() == TestDB::kMockInMemory) {
      // LE, not EQ, because storage cost of the fibonacci contract is nonzero
      EXPECT_LE(from_xdr(token_item_opt.value().nnint()), 1'000'000'000);
    }
    else {
      warn("Hogdb storage cost accounting is backwards");
    }
  }
}

// Test that the genesis block unfurls correctly both when proposing and
// validating. This catches an early bug in which contract deployments and
// creations were not included in the db blocktx workflow
TEST_P(GenesisBlockUnfurlTest, ProposeThenValidateOK)
{
  auto fibonacci_contract = load_contract("fibonacci.wasm");
  auto caller_contract = load_contract("fibonacci_call.wasm");

  auto fibonacci_nonce = make_nonce(0);
  auto caller_nonce = make_nonce(1);

  auto expected_fibonacci_address =
      GenesisBlockLoader::calculate_deployed_address(sha256(
                                                         *fibonacci_contract),
                                                     fibonacci_nonce);
  auto expected_caller_address =
      GenesisBlockLoader::calculate_deployed_address(sha256(*caller_contract),
                                                     caller_nonce);

  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;
  using caller_meta_t = ContractTraitMeta<fibonacci_call::FibonacciCaller>;

  GenesisBlockBuilder builder;
  builder.add_initial_config(fibonacci_contract, fibonacci_nonce,
                             fibonacci_meta_t::entrypoint<"fibonacci">(),
                             fibonacci_meta_t::make_calldata<"fibonacci">(
                                 uint32_t{0}),
                             int128_t{1'000'000'000});
  builder.add_initial_config(caller_contract, caller_nonce,
                             caller_meta_t::entrypoint<"initialize">(),
                             caller_meta_t::make_calldata<"initialize">(
                                 expected_fibonacci_address),
                             int128_t{1'000'000'000});
  auto block = builder.build();

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  EXPECT_EQ(unfurl_txns.size(), 2);

  run_genesis_initialization(block);

  // run the unfurl transactions

  for (auto const &tx : unfurl_txns) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx, wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // contracts should not be created yet
  EXPECT_FALSE(lookup_contract_from_db(sha256(*fibonacci_contract)));
  EXPECT_FALSE(lookup_contract_from_db(sha256(*caller_contract)));

  // run the unfurl transactions

  for (auto const &tx : unfurl_txns) {
    auto handler = make_new_system_call_handler(tx, wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  advance_block();

  // contracts should now exist
  EXPECT_TRUE(lookup_contract_from_db(sha256(*fibonacci_contract)));
  EXPECT_TRUE(lookup_contract_from_db(sha256(*caller_contract)));

  auto caller_client = ContractTraitClient<fibonacci_call::FibonacciCaller>(
      expected_caller_address);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation =
                                caller_client.make_invocation<"call_fibonacci">(
                                    UINT64_MAX, uint32_t{5}, UINT64_MAX)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
}

TEST_P(GenesisBlockUnfurlTest, DoubleLoadError)
{
  GenesisBlock genesis_block{};

  auto r = get_coord_client().dbsched_load_genesis_block(genesis_block)();
  ASSERT_EQ(r, DbSchedResult::DbSchedResultOK);
  r = get_coord_client().dbsched_load_genesis_block(genesis_block)();
  ASSERT_EQ(r, DbSchedResult::DbSchedResultError);
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, GenesisBlockUnfurlTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
