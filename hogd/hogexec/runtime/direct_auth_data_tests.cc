#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

class DirectAuthDataTest : public ::testing::TestWithParam<TestDB>,
                           public NativeContractTestFramework {
public:
  DirectAuthDataTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  void SetUp() override { cost_model_ = get_default_cost_model(); }

  static auto make_pk_config(Weight w)
  {
    auto [pk, _] = ed25519_keygen();
    PublicKey pk_wrapper(CryptographicScheme::Ed25519);
    pk_wrapper.ed25519() = pk;
    return x23::Ptr<PublicKeyConfig>(
        new PublicKeyConfig{.key = pk_wrapper, .max_weight = w});
  }

  static auto make_auth_data_delta(const AuthDataChangeArgs &args)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeAuthDataChange);
    // For simplicity, we'll create a basic auth data change
    // In a real scenario, this would be more complex
    delta.auth_data_change() = args;
    return delta;
  }

  static auto make_delete_delta()
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeDelete);
    return delta;
  }

  void check_balance(ContractAddress contract_addr, int128_t expected_balance)
  {
    auto bal = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(contract_addr));
    ASSERT_TRUE(bal.has_value());
    ASSERT_EQ(from_xdr(bal.value().nnint()), expected_balance);
  }

  // check that the two addresses have the same balance,
  // and that that balance is less than or equal to
  // the initial balance
  void check_balances(ContractAddress contract_addr,
                      ContractAddress reference_addr, int128_t expected_balance)
  {
    // check that costs are reflected in the balance
    check_balance(contract_addr, expected_balance);
    check_balance(reference_addr, expected_balance);
  }

  const StorageCostModel *cost_model_;
};

TEST_P(DirectAuthDataTest, BasicAuthDataCreation)
{
  // Test basic creation of auth data
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create basic auth data
  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});

  auto tx1 = make_new_per_tx_db_proxy();
  // Note: This is a simplified test - actual auth data changes would use
  // the proper AuthDataChangeArgs structure
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(tx1.commit_transaction({}));
}

TEST_P(DirectAuthDataTest, AuthDataDoubleWrite)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "Auth data shared locks not supported in MockInMemory";
    return;
  }
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(tx1.commit_transaction({}));

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(tx2.commit_transaction({}));

  advance_block();
  check_balances(contract_addr, contract_addr,
                 kInitialBalance - (cost_model_->get_auth_data_cost(1)));
  auto item = lookup_storage_item_from_db(
      make_account_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto db_auth_data = item->auth_data();
  ASSERT_TRUE(db_auth_data.public_keys[0]);
  ASSERT_EQ(db_auth_data.public_keys[0]->key,
            auth_data.auth_data_change_signers[0].public_key->key);
}

TEST_P(DirectAuthDataTest, AuthDataStorageCosts)
{
  // Test that auth data creation incurs proper storage costs
  auto contract_addr = make_contract_address(0);
  auto contract_addr_ref = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(contract_addr_ref, kInitialBalance);

  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auth_data.auth_data_change_signers.push_back({1, make_pk_config(Weight{2})});

  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_2.has_value());
  ASSERT_TRUE(r1_2.value());
  ASSERT_TRUE(tx1.commit_transaction({}));

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr_ref),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());
  ASSERT_TRUE(tx2.commit_transaction({}));

  advance_block();
  check_balances(contract_addr, contract_addr_ref,
                 kInitialBalance - (cost_model_->get_auth_data_cost(2)));
  auto item = lookup_storage_item_from_db(
      make_account_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto db_auth_data = item->auth_data();
  ASSERT_TRUE(db_auth_data.public_keys[0]);
  ASSERT_TRUE(db_auth_data.public_keys[1]);
  ASSERT_EQ(db_auth_data.public_keys[0]->key,
            auth_data.auth_data_change_signers[0].public_key->key);
  ASSERT_EQ(db_auth_data.public_keys[1]->key,
            auth_data.auth_data_change_signers[1].public_key->key);
}

TEST_P(DirectAuthDataTest, AuthDataNonconflicting)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  AuthDataChangeArgs auth_data1{};
  auth_data1.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});
  AuthDataChangeArgs auth_data2{};
  auth_data2.auth_data_change_signers.push_back({1, make_pk_config(Weight{2})});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data1),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(tx1.commit_transaction({}));

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(tx2.commit_transaction({}));
  advance_block();
  check_balance(contract_addr,
                kInitialBalance - (cost_model_->get_auth_data_cost(2)));

  auto item = lookup_storage_item_from_db(
      make_account_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto db_auth_data = item->auth_data();
  ASSERT_TRUE(db_auth_data.public_keys[0]);
  ASSERT_TRUE(db_auth_data.public_keys[1]);
  ASSERT_EQ(db_auth_data.public_keys[0]->key,
            auth_data1.auth_data_change_signers[0].public_key->key);
  ASSERT_EQ(db_auth_data.public_keys[1]->key,
            auth_data2.auth_data_change_signers[0].public_key->key);
}

TEST_P(DirectAuthDataTest, AuthDataTryInvoke)
{
  auto contract_addr = make_contract_address(0);
  auto contract_addr_ref = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(contract_addr_ref, kInitialBalance);

  AuthDataChangeArgs auth_data1{};
  auth_data1.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});
  AuthDataChangeArgs auth_data2{};
  auth_data2.auth_data_change_signers.push_back({1, make_pk_config(Weight{2})});
  AuthDataChangeArgs auth_data3{};
  auth_data3.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});
  auth_data3.auth_data_change_signers.push_back({2, make_pk_config(Weight{3})});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data1),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());

  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());

  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data3),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_3 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_3.has_value());

  ASSERT_TRUE(tx1.pop_subtransactions(1).has_value());
  ASSERT_TRUE(tx1.commit_transaction({}));

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr_ref),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(tx2.commit_transaction({}));

  advance_block();
  check_balances(contract_addr, contract_addr_ref,
                 kInitialBalance - (cost_model_->get_auth_data_cost(1)));

  auto item = lookup_storage_item_from_db(
      make_account_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto db_auth_data = item->auth_data();
  ASSERT_FALSE(db_auth_data.public_keys[0]);
  ASSERT_TRUE(db_auth_data.public_keys[1]);
  ASSERT_FALSE(db_auth_data.public_keys[2]);
  ASSERT_EQ(db_auth_data.public_keys[1]->key,
            auth_data2.auth_data_change_signers[0].public_key->key);

  auto item_ref = lookup_storage_item_from_db(
      make_account_auth_data_addrkey(contract_addr_ref));
  ASSERT_TRUE(item_ref.has_value());
  auto db_auth_data_ref = item_ref->auth_data();

  ASSERT_EQ(db_auth_data, db_auth_data_ref);
}

TEST_P(DirectAuthDataTest, AuthDataConcurrentLocks)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "Auth data shared locks not supported in MockInMemory";
    return;
  }
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto pk1 = make_pk_config(Weight{1});
  AuthDataChangeArgs auth_data1{};
  auth_data1.auth_data_change_signers.push_back({0, pk1});
  AuthDataChangeArgs auth_data2{};
  auth_data2.auth_data_change_signers.push_back({0, make_pk_config(Weight{2})});

  AuthDataChangeArgs auth_data_conflict{};
  auth_data_conflict.auth_data_change_signers.push_back({0, nullptr});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data1),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data1),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_2.has_value());
  ASSERT_FALSE(r1_2.value());

  ASSERT_TRUE(tx2.pop_subtransactions(1).has_value());

  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_3 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_3.has_value());
  ASSERT_TRUE(r1_3.value());

  tx2.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2_2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_2.has_value());
  ASSERT_TRUE(r2_2.value());

  tx2.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data_conflict),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2_3 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_3.has_value());
  ASSERT_FALSE(r2_3.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  advance_block();
  check_balance(contract_addr,
                kInitialBalance - (cost_model_->get_auth_data_cost(1)));
}

TEST_P(DirectAuthDataTest, AuthDataMarginalCosts)
{
  // This test would fail if tx2 were to be charged more than the marginal cost
  // of adding two new keys.
  auto contract_addr = make_contract_address(0);
  // NOLINTNEXTLINE(readability-identifier-naming)
  const int128_t kInitialBalance = cost_model_->get_auth_data_cost(4);
  setup_grant_storage_token(contract_addr, kInitialBalance);

  {
    // Initialize the account with 1 key
    AuthDataChangeArgs auth_data0{};
    auth_data0.auth_data_change_signers.push_back(
        {0, make_pk_config(Weight{1})});

    auto tx0 = make_new_per_tx_db_proxy();
    tx0.add_delta(make_account_auth_data_addrkey(contract_addr),
                  make_auth_data_delta(auth_data0),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r0 = tx0.try_push_current_subtransaction();
    ASSERT_TRUE(r0.has_value());
    ASSERT_TRUE(r0.value());
    ASSERT_TRUE(tx0.commit_transaction({}).has_value());
    advance_block();
    check_balance(contract_addr,
                  kInitialBalance - (cost_model_->get_auth_data_cost(1)));
  }

  AuthDataChangeArgs auth_data1{};
  auth_data1.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});
  auth_data1.auth_data_change_signers.push_back({1, make_pk_config(Weight{2})});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data1),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  AuthDataChangeArgs auth_data2{};
  auth_data2.auth_data_change_signers.push_back({2, make_pk_config(Weight{1})});
  auth_data2.auth_data_change_signers.push_back({3, make_pk_config(Weight{2})});

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  advance_block();
  check_balance(contract_addr, 0);
}

TEST_P(DirectAuthDataTest, AuthDataMarginalCosts2)
{
  // This test would fail if tx2 were to be charged more than the marginal cost
  // of adding two new keys.
  auto contract_addr = make_contract_address(0);
  // NOLINTNEXTLINE(readability-identifier-naming)
  const int128_t kInitialBalance = cost_model_->get_auth_data_cost(4);
  setup_grant_storage_token(contract_addr, kInitialBalance);

  {
    // Initialize the account with 1 key
    AuthDataChangeArgs auth_data0{};
    auth_data0.auth_data_change_signers.push_back(
        {0, make_pk_config(Weight{1})});

    auto tx0 = make_new_per_tx_db_proxy();
    tx0.add_delta(make_account_auth_data_addrkey(contract_addr),
                  make_auth_data_delta(auth_data0),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r0 = tx0.try_push_current_subtransaction();
    ASSERT_TRUE(r0.has_value());
    ASSERT_TRUE(r0.value());
    ASSERT_TRUE(tx0.commit_transaction({}).has_value());
    advance_block();
    check_balance(contract_addr,
                  kInitialBalance - (cost_model_->get_auth_data_cost(1)));
  }

  AuthDataChangeArgs auth_data2{};
  auth_data2.auth_data_change_signers.push_back({2, make_pk_config(Weight{1})});
  auth_data2.auth_data_change_signers.push_back({3, make_pk_config(Weight{2})});

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data2),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // reverse order from above AuthDataMarginalCosts
  AuthDataChangeArgs auth_data1{};
  auth_data1.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});
  auth_data1.auth_data_change_signers.push_back({1, make_pk_config(Weight{2})});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data1),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  advance_block();
  check_balance(contract_addr, 0);
}

TEST_P(DirectAuthDataTest, AuthDataInterleavings)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "Auth data shared locks not supported in MockInMemory";
    return;
  }
  AuthDataChangeArgs auth_data_init{};
  auth_data_init.auth_data_change_signers.push_back(
      {0, make_pk_config(Weight{1})});
  auth_data_init.auth_data_change_signers.push_back(
      {1, make_pk_config(Weight{2})});
  for (uint32_t i = 2; i < MAX_SIGNERS; i++) {
    auth_data_init.auth_data_change_signers.push_back({i, nullptr});
  }
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto initialize = [&] {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_account_auth_data_addrkey(contract_addr),
                 make_auth_data_delta(auth_data_init),
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx.commit_transaction({}).has_value());
    advance_block();
  };

  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    model::RunnableActionSet actions(work_queue);

    initialize();

    actions.add_action(
        [](model::RunnableActionSet &set, auto self) -> model::Async {
          auto contract_addr = make_contract_address(0);

          auto auth_data1 = AuthDataChangeArgs{};
          auth_data1.auth_data_change_signers.push_back({0, nullptr});
          auth_data1.auth_data_change_signers.push_back(
              {5, make_pk_config(Weight{2})});

          co_await set.bg();

          auto tx = self->make_new_per_tx_db_proxy();
          tx.add_delta(make_account_auth_data_addrkey(contract_addr),
                       make_auth_data_delta(auth_data1),
                       std::to_underlying(StorageItemRPCFlags::kStorageNone));
          auto r1 = tx.try_push_current_subtransaction();
          if (!r1.has_value() || !r1.value()) {
            throw std::runtime_error(
                "try_push_current_subtransaction 1-1 error");
          }
          co_await set.bg();

          auto auth_data2 = AuthDataChangeArgs{};
          auth_data2.auth_data_change_signers.push_back(
              {1, make_pk_config(Weight{2})});
          auth_data2.auth_data_change_signers.push_back(
              {6, make_pk_config(Weight{3})});

          tx.add_delta(make_account_auth_data_addrkey(contract_addr),
                       make_auth_data_delta(auth_data2),
                       std::to_underlying(StorageItemRPCFlags::kStorageNone));
          auto r2 = tx.try_push_current_subtransaction();
          if (!r2.has_value() || !r2.value()) {
            throw std::runtime_error(
                "try_push_current_subtransaction 1-2 error");
          }
          co_await set.bg();

          if (!tx.commit_transaction({}).has_value()) {
            throw std::runtime_error("commit_transaction error");
          }

          co_return;
        },
        this);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self) -> model::Async {
          auto contract_addr = make_contract_address(0);

          auto auth_data3 = AuthDataChangeArgs{};
          auth_data3.auth_data_change_signers.push_back(
              {2, make_pk_config(Weight{3})});
          auth_data3.auth_data_change_signers.push_back({0, nullptr});

          co_await set.bg();

          auto tx = self->make_new_per_tx_db_proxy();
          tx.add_delta(make_account_auth_data_addrkey(contract_addr),
                       make_auth_data_delta(auth_data3),
                       std::to_underlying(StorageItemRPCFlags::kStorageNone));
          auto r3 = tx.try_push_current_subtransaction();
          if (!r3.has_value() || !r3.value()) {
            throw std::runtime_error(
                "try_push_current_subtransaction 2-1 error");
          }
          co_await set.bg();

          if (!tx.commit_transaction({}).has_value()) {
            throw std::runtime_error("commit_transaction error");
          }

          co_return;
        },
        this);

    advance_block();

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    advance_block();

    auto item = lookup_storage_item_from_db(
        make_account_auth_data_addrkey(contract_addr));
    ASSERT_TRUE(item.has_value());
    auto db_auth_data = item->auth_data();
    ASSERT_FALSE(db_auth_data.public_keys[0]);
    ASSERT_TRUE(db_auth_data.public_keys[1]);
    ASSERT_TRUE(db_auth_data.public_keys[2]);
    ASSERT_TRUE(db_auth_data.public_keys[6]);

    check_balance(contract_addr,
                  kInitialBalance - (cost_model_->get_auth_data_cost(3)));

    work_queue.advance_cursor();
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectAuthDataTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
