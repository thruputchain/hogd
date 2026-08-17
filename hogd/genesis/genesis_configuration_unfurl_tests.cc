#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <set>

#include "address_auth.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "built_contracts/fibonacci_call.h"
#include "built_contracts/manual_revert_test.h"
#include "consensus.hh"
#include "db_sched.hh"
#include "exec_result.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class GenesisConfigurationUnfurlTest : public ::testing::Test,
                                       public NativeContractTestFramework {
public:
  GenesisConfigurationUnfurlTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, false)
  {}

  void SetUp() override { runtime_context_ = make_hog_exec_context(); }

protected:
  std::unique_ptr<RuntimeContext> runtime_context_;

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

  static int128_t
  genesis_deployer_storage_costs(StorageCostModel const &cost_model)
  {
    AddressAuthData deployer_auth_data{};
    deployer_auth_data.required_weight = 0;
    deployer_auth_data.last_written_height = 0;
    StorageItemWrapper deployer_auth_item(deployer_auth_data, &cost_model);
    // The unfurl transactions are invoked on the initial token claim
    // precompile, which is exempt from the replay cache, so they consume no
    // replay-cache entry and incur no extra cost here.
    return deployer_auth_item.storage_cost(&cost_model) +
           cost_model.get_balance_cost();
  }

  std::set<Uniquifier> uniquifiers_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(GenesisConfigurationUnfurlTest, LaunchFibonacciContractAndCaller)
{
  auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
  auto caller_contract = load_rv32_contract("fibonacci_call.rv32a");

  auto fibonacci_nonce = make_nonce(0);
  auto caller_nonce = make_nonce(1);

  auto expected_fibonacci_address =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*fibonacci_contract), fibonacci_nonce);
  auto expected_caller_address =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*caller_contract), caller_nonce);

  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;
  using caller_meta_t = ContractTraitMeta<fibonacci_call::FibonacciCaller>;

  GenesisConfigurationBuilder builder;
  builder.add_initial_config(fibonacci_contract, fibonacci_nonce,
                             fibonacci_meta_t::entrypoint<u8"fibonacci">(),
                             fibonacci_meta_t::make_calldata<u8"fibonacci">(
                                 uint32_t{0}),
                             int128_t{1'000'000'000});
  builder.add_initial_config(caller_contract, caller_nonce,
                             caller_meta_t::entrypoint<u8"initialize">(),
                             caller_meta_t::make_calldata<u8"initialize">(
                                 expected_fibonacci_address),
                             int128_t{1'000'000'000});
  auto genesis_configuration =
      builder.build_or_panic(StorageCostModel::default_cost_model());

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  EXPECT_EQ(unfurl_txns.size(), 2);

  run_genesis_initialization(genesis_configuration);

  // run the unfurl transactions

  for (auto const &tx : unfurl_txns) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx.tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                    ProposedTransaction{.signed_tx = tx})());
  }

  advance_block();

  // Should be able to invoke the fibonacci caller contract now
  auto caller_client = ContractTraitClient<fibonacci_call::FibonacciCaller>(
      expected_caller_address);

  {
    auto tx = Transaction{.invocation =
                              caller_client.make_invocation<u8"call_fibonacci">(
                                  UINT64_MAX, uint32_t{5}, UINT64_MAX),
                          .uniquifier = make_new_uniquifier()};
    auto handler = make_new_system_call_handler(tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(
        ExecResult::ExecResultOK,
        ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());
  }

  // check the storage
  {
    auto token_addrkey =
        make_free_native_token_balance_addrkey(expected_caller_address);
    auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
    ASSERT_TRUE(token_item_opt.has_value());
    ASSERT_EQ(token_item_opt.value().type(),
              StorageItemType::kItemTypeNonnegativeInt);
    EXPECT_LE(from_xdr(token_item_opt.value().nnint()), 1'000'000'000);
  }

  {
    auto token_addrkey =
        make_free_native_token_balance_addrkey(expected_fibonacci_address);
    auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
    ASSERT_TRUE(token_item_opt.has_value());
    ASSERT_EQ(token_item_opt.value().type(),
              StorageItemType::kItemTypeNonnegativeInt);
    // LE, not EQ, because storage cost of the fibonacci contract is nonzero
    EXPECT_LE(from_xdr(token_item_opt.value().nnint()), 1'000'000'000);
  }
}

// Test that the genesis block unfurls correctly both when proposing and
// validating. This catches an early bug in which contract deployments and
// creations were not included in the db blocktx workflow
TEST_F(GenesisConfigurationUnfurlTest, ProposeThenValidateOK)
{
  auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
  auto caller_contract = load_rv32_contract("fibonacci_call.rv32a");

  auto fibonacci_nonce = make_nonce(0);
  auto caller_nonce = make_nonce(1);

  auto expected_fibonacci_address =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*fibonacci_contract), fibonacci_nonce);
  auto expected_caller_address =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*caller_contract), caller_nonce);

  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;
  using caller_meta_t = ContractTraitMeta<fibonacci_call::FibonacciCaller>;

  GenesisConfigurationBuilder builder;
  builder.add_initial_config(fibonacci_contract, fibonacci_nonce,
                             fibonacci_meta_t::entrypoint<u8"fibonacci">(),
                             fibonacci_meta_t::make_calldata<u8"fibonacci">(
                                 uint32_t{0}),
                             int128_t{1'000'000'000});
  builder.add_initial_config(caller_contract, caller_nonce,
                             caller_meta_t::entrypoint<u8"initialize">(),
                             caller_meta_t::make_calldata<u8"initialize">(
                                 expected_fibonacci_address),
                             int128_t{1'000'000'000});
  auto genesis_configuration =
      builder.build_or_panic(StorageCostModel::default_cost_model());

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  EXPECT_EQ(unfurl_txns.size(), 2);

  run_genesis_initialization(genesis_configuration);

  // run the unfurl transactions

  for (auto const &tx : unfurl_txns) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx.tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                    ProposedTransaction{.signed_tx = tx})());
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // contracts should not be created yet
  EXPECT_FALSE(lookup_contract_from_db(sha256(*fibonacci_contract)));
  EXPECT_FALSE(lookup_contract_from_db(sha256(*caller_contract)));

  // run the unfurl transactions
  for (auto const &tx : unfurl_txns) {
    auto handler = make_new_system_call_handler(tx.tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                    ProposedTransaction{.signed_tx = tx})());
  }

  advance_block();

  // contracts should now exist
  EXPECT_TRUE(lookup_contract_from_db(sha256(*fibonacci_contract)));
  EXPECT_TRUE(lookup_contract_from_db(sha256(*caller_contract)));

  auto caller_client = ContractTraitClient<fibonacci_call::FibonacciCaller>(
      expected_caller_address);

  {
    auto tx = Transaction{.invocation =
                              caller_client.make_invocation<u8"call_fibonacci">(
                                  UINT64_MAX, uint32_t{5}, UINT64_MAX),
                          .uniquifier = make_new_uniquifier()};
    auto handler = make_new_system_call_handler(tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(
        ExecResult::ExecResultOK,
        ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());
  }
}

TEST_F(GenesisConfigurationUnfurlTest, DoubleLoadError)
{
  GenesisConfiguration genesis_configuration{
      .default_wallet =
          GenesisConfigurationLoader::native_default_wallet_hash()};
  ConsensusValue genesis_value{};
  genesis_value.proposal_header.block_height = 0;

  auto r = get_coord_client().dbsched_load_genesis_configuration(
      genesis_configuration, genesis_value);
  ASSERT_EQ(r, DbSchedResult::DbSchedResultOK);
  r = get_coord_client().dbsched_load_genesis_configuration(
      genesis_configuration, genesis_value);
  ASSERT_EQ(r, DbSchedResult::DbSchedResultError);
}

TEST_F(GenesisConfigurationUnfurlTest,
       AllocateEntireTokenSupplyMultipleContracts)
{
  auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
  auto caller_contract = load_rv32_contract("fibonacci_call.rv32a");

  auto fibonacci_nonce = make_nonce(0);
  auto caller_nonce = make_nonce(1);

  auto expected_fibonacci_address =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*fibonacci_contract), fibonacci_nonce);
  auto expected_caller_address =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*caller_contract), caller_nonce);

  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;
  using caller_meta_t = ContractTraitMeta<fibonacci_call::FibonacciCaller>;

  auto cost_model = StorageCostModel::default_cost_model();
  GenesisConfigurationBuilder builder;
  builder.add_initial_config(fibonacci_contract, fibonacci_nonce,
                             fibonacci_meta_t::entrypoint<u8"fibonacci">(),
                             fibonacci_meta_t::make_calldata<u8"fibonacci">(
                                 uint32_t{0}),
                             int128_t{1'000'000'000});
  builder.add_initial_config(
      caller_contract, caller_nonce,
      caller_meta_t::entrypoint<u8"initialize">(),
      caller_meta_t::make_calldata<u8"initialize">(expected_fibonacci_address),
      GenesisConfigurationLoader::initial_hog_token_supply() -
          int128_t{1'000'000'000} - genesis_deployer_storage_costs(cost_model));
  auto block = builder.build_or_panic(cost_model);

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  EXPECT_EQ(unfurl_txns.size(), 2);

  run_genesis_initialization(block);

  // run the unfurl transactions

  for (auto const &tx : unfurl_txns) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx.tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                    ProposedTransaction{.signed_tx = tx})());
  }

  advance_block();

  // contracts should now exist
  EXPECT_TRUE(lookup_contract_from_db(sha256(*fibonacci_contract)));
  EXPECT_TRUE(lookup_contract_from_db(sha256(*caller_contract)));

  auto caller_client = ContractTraitClient<fibonacci_call::FibonacciCaller>(
      expected_caller_address);

  {
    auto tx = Transaction{.invocation =
                              caller_client.make_invocation<u8"call_fibonacci">(
                                  UINT64_MAX, uint32_t{5}, UINT64_MAX),
                          .uniquifier = make_new_uniquifier()};
    auto handler = make_new_system_call_handler(tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(
        ExecResult::ExecResultOK,
        ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());
  }
}

TEST_F(GenesisConfigurationUnfurlTest, AllocateEntireTokenSupplyOneAddress)
{
  auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");

  auto fibonacci_nonce = make_nonce(0);

  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;

  auto cost_model = StorageCostModel::default_cost_model();

  GenesisConfigurationBuilder builder;
  builder.add_initial_config(
      fibonacci_contract, fibonacci_nonce,
      fibonacci_meta_t::entrypoint<u8"fibonacci">(),
      fibonacci_meta_t::make_calldata<u8"fibonacci">(uint32_t{0}),
      GenesisConfigurationLoader::initial_hog_token_supply() -
          genesis_deployer_storage_costs(cost_model));
  auto genesis_configuration = builder.build_or_panic(cost_model);

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  EXPECT_EQ(unfurl_txns.size(), 1);

  run_genesis_initialization(genesis_configuration);

  // run the unfurl transactions

  for (auto const &tx : unfurl_txns) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx.tx, *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                    ProposedTransaction{.signed_tx = tx})());
  }

  advance_block();

  // contracts should now exist
  EXPECT_TRUE(lookup_contract_from_db(sha256(*fibonacci_contract)));
}

TEST_F(GenesisConfigurationUnfurlTest, InitialTokenClaimPropagatesReverts)
{
  // Assert that if a contract reverts during the initial token claim's init
  // call, the entire transaction reverts. In the actual implementation
  // long-term, we _could_ build some kind of recovery fallback, if we
  // initialize the chain with bad contracts.  But it would be much better to
  // catch these errors earlier, during testing.
  using manual_revert_meta_t =
      ContractTraitMeta<manual_revert_test::ManualRevertTest>;

  auto manual_revert_contract = manual_revert_meta_t::load_rv32();
  auto manual_revert_nonce = make_nonce(0);

  GenesisConfigurationBuilder builder;
  builder.add_initial_config(
      manual_revert_contract, manual_revert_nonce,
      manual_revert_meta_t::entrypoint<u8"test_basic_manual_revert">(),
      manual_revert_meta_t::make_calldata<u8"test_basic_manual_revert">(true),
      int128_t{1'000'000'000});
  auto genesis_configuration =
      builder.build_or_panic(StorageCostModel::default_cost_model());

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  ASSERT_EQ(unfurl_txns.size(), 1);

  run_genesis_initialization(genesis_configuration);

  // run the unfurl transactions

  auto const &stx = unfurl_txns[0];
  auto handler = make_new_system_call_handler(stx.tx, *runtime_context_);
  auto res = handler.root_invoke();
  ASSERT_FALSE(res.is_root_success());
}

TEST_F(GenesisConfigurationUnfurlTest,
       TracedInitialTokenClaimDirectChildInvocation)
{
  auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
  auto fibonacci_nonce = make_nonce(0);

  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;

  GenesisConfigurationBuilder builder;
  builder.add_initial_config(fibonacci_contract, fibonacci_nonce,
                             fibonacci_meta_t::entrypoint<u8"fibonacci">(),
                             fibonacci_meta_t::make_calldata<u8"fibonacci">(
                                 uint32_t{0}),
                             int128_t{1'000'000'000});
  auto genesis_configuration =
      builder.build_or_panic(StorageCostModel::default_cost_model());

  auto unfurl_txns = builder.make_unfurl_genesis_transactions();
  ASSERT_EQ(unfurl_txns.size(), 1);

  run_genesis_initialization(genesis_configuration);

  auto handler =
      make_new_system_call_handler(unfurl_txns[0].tx, *runtime_context_);
  handler.enable_tracing();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = unfurl_txns[0]})());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
