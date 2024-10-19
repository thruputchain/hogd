#include <gtest/gtest.h>

#include <sys/socket.h>

#include <cstdint>
#include <cstring>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/softstate/local_softstate_map.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

class ERC20Test : public ::testing::Test, public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();
    test::RuntimeTestFramework::deploy_genesis_block();
  }
};

namespace {

SignedTransaction make_deploy_wallet_transaction(SHA256Hash const& h_wallet, std::shared_ptr<const Contract> c_wallet,
  uint64_t nonce, 
  Ed25519PublicKey const& pk, ContractAddress const& erc20_addr) {

  SignedTransaction wallet_stx;
  wallet_stx.tx.invocation.invoked_address = test::RuntimeTestFramework::deployer_address_;
  wallet_stx.tx.invocation.method_name = 1;
  wallet_stx.tx.invocation.gas_limit = UINT64_MAX;
  wallet_stx.tx.invocation.calldata = 
    test::make_calldata(h_wallet, test::get_sdk_name("initialize"),
     pk, erc20_addr);
  wallet_stx.nonce = nonce;
  wallet_stx.tx.contracts_to_deploy.push_back(*c_wallet);

  return wallet_stx;
}

SignedTransaction make_deploy_erc20_noinit(SHA256Hash const& h_erc20, std::shared_ptr<const Contract> c_erc20) {

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = test::RuntimeTestFramework::deployer_address_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = 
    test::make_calldata(h_erc20);
  stx.tx.contracts_to_deploy.push_back(*c_erc20);
  stx.nonce = 0;

  return stx;
}

SignedTransaction make_initialize_erc20(ContractAddress const& owner, ContractAddress const& erc20_address) {

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = erc20_address;
  stx.tx.invocation.method_name = test::get_sdk_name("initialize");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = 
    test::make_calldata(owner);
  stx.nonce = 0;

  return stx;
}

SignedTransaction make_mint_transaction(ContractAddress const& manager_wallet, ContractAddress const& to) {
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = manager_wallet;
  stx.tx.invocation.method_name = test::get_sdk_name("mint");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = 
    test::make_calldata(to, static_cast<int64_t>(10000));
  stx.nonce = 0;

  return stx;
}

SignedTransaction make_check_balance_transaction(ContractAddress const& wallet, int64_t value) {
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = wallet;
  stx.tx.invocation.method_name = test::get_sdk_name("test_expect_balance");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = 
    test::make_calldata(value);
  stx.nonce = 0;

  return stx;
}

SignedTransaction make_transfer_transaction(ContractAddress const& sender, ContractAddress const& to, int64_t amount)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = sender;
  stx.tx.invocation.method_name = test::get_sdk_name("send_payment");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = 
    test::make_calldata(to, amount);
  stx.nonce = 0;

  return stx;
}

}

TEST_F(ERC20Test, InitializeAndMint)
{
  auto [h_erc20, c_erc20] = test::load_built_rs_contract("erc20.wasm");
  auto [h_wallet, c_wallet] = test::load_built_rs_contract("simple_wallet.wasm");

  ContractAddress erc20_address;
  ContractAddress manager_address;

  std::vector<ContractAddress> wallets;

  // Create an ERC20 contract
  {
    auto deploy_erc20_tx = make_deploy_erc20_noinit(h_erc20, c_erc20);

    SystemCallHandler handler(deploy_erc20_tx, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    erc20_address = SystemCallHandler::static_get_deploy_address(
        deployer_address_, handler.get_source_tx_hash(), h_erc20);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  // Create a wallet contract that controls minting on the erc20
  {
    auto deploy_manager_tx = make_deploy_wallet_transaction(h_wallet, c_wallet,
      0, Ed25519PublicKey{}, erc20_address);

    SystemCallHandler handler(deploy_manager_tx, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    manager_address = SystemCallHandler::static_get_deploy_address(
        deployer_address_, handler.get_source_tx_hash(), h_wallet);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  // Create 10 user wallets for the erc20
  for (int i = 0; i < 10; i++)
  {
    auto deploy_wallet_tx = make_deploy_wallet_transaction(h_wallet, c_wallet,
      i + 1, Ed25519PublicKey{}, erc20_address);

    SystemCallHandler handler(deploy_wallet_tx, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    auto addr = SystemCallHandler::static_get_deploy_address(
        deployer_address_, handler.get_source_tx_hash(), h_wallet);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());

    wallets.push_back(addr);
  }

  commit_round();

  // Initialize the erc20 (set the owner to be the wallet above)
  {
    auto init_erc20_tx = make_initialize_erc20(manager_address, erc20_address);

    SystemCallHandler handler(init_erc20_tx, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  commit_round();

  // Mint money for each wallet
  for (auto i = 0U; i < wallets.size(); i++)
  {
    auto mint_tx = make_mint_transaction(manager_address, wallets[i]);

    SystemCallHandler handler(mint_tx, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  commit_round();

  // check balances are all set at 10000, from the mint
  for (auto i = 0U; i < wallets.size(); i++)
  {
    auto check_balance = make_check_balance_transaction(wallets[i], 10000);

    SystemCallHandler handler(check_balance, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  // Each wallet pays some money to the manager
  for (auto i = 0U; i < wallets.size(); i++)
  {
    auto payment = make_transfer_transaction(wallets[i], manager_address, (i+1) * 100);

    SystemCallHandler handler(payment, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  // Check balances don't update yet (block not yet committed)
  for (auto i = 0U; i < wallets.size(); i++)
  {
    auto check_balance = make_check_balance_transaction(wallets[i], 10000);

    SystemCallHandler handler(check_balance, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  commit_round();

  // Check balance updates
  for (auto i = 0U; i < wallets.size(); i++)
  {
    auto check_balance = make_check_balance_transaction(wallets[i], 10000 - (i+1)*100);

    SystemCallHandler handler(check_balance, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  // Check manager received funds
  {
    auto check_balance = make_check_balance_transaction(manager_address, 5500);

    SystemCallHandler handler(check_balance, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }
}

} // namespace hogd
