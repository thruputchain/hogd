#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/erc20_test_utils.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "storage_interface.hh"
#include "transaction.hh"
#include "types.hh"

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

SignedTransaction
make_check_balance_transaction(ContractAddress const &wallet, int64_t value)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = wallet;
  stx.tx.invocation.method_name = test::get_sdk_name("test_expect_balance");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata(value);
  stx.nonce = 0;

  return stx;
}

} // namespace

TEST_F(ERC20Test, InitializeAndMint)
{
  ContractAddress erc20_address;
  ContractAddress manager_address;

  std::vector<ContractAddress> wallets;

  // NOLINTBEGIN(bugprone-unchecked-optional-access)

  // Create an ERC20 contract
  auto r = test::erc20::deploy_erc20_noinit(*this);
  ASSERT_TRUE(r);
  erc20_address = *r;

  // Create a wallet contract that controls minting on the erc20
  r = test::erc20::deploy_and_initialize_wallet(*this, erc20_address,
                                                0); // nonce 0
  ASSERT_TRUE(r);
  manager_address = *r;

  // Create 10 user wallets for the erc20
  for (int i = 0; i < 10; i++) {

    r = test::erc20::deploy_and_initialize_wallet(*this, erc20_address,
                                                  i + 1); // nonce i+1
    ASSERT_TRUE(r);
    wallets.push_back(*r);
  }

  // NOLINTEND(bugprone-unchecked-optional-access)

  commit_round();

  // Initialize the erc20 (set the owner to be the wallet above)
  ASSERT_TRUE(
      test::erc20::initialize_erc20(*this, manager_address, erc20_address));
  commit_round();

  // Mint money for each wallet
  for (auto const &wallet : wallets) {
    ASSERT_TRUE(test::erc20::mint_to(*this, manager_address, wallet, 10'000));
  }

  commit_round();

  // check balances are all set at 10000, from the mint
  for (auto const &wallet : wallets) {
    auto check_balance = make_check_balance_transaction(wallet, 10000);

    SystemCallHandler handler(check_balance, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  // Each wallet pays some money to the manager
  for (auto i = 0U; i < wallets.size(); i++) {
    ASSERT_TRUE(test::erc20::send_payment(*this, wallets[i], manager_address,
                                          (i + 1) * 100));
  }

  // Check balances don't update yet (block not yet committed)
  for (auto const &wallet : wallets) {
    auto check_balance = make_check_balance_transaction(wallet, 10000);

    SystemCallHandler handler(check_balance, *snapshot_cache_);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  commit_round();

  // Check balance updates
  for (auto i = 0U; i < wallets.size(); i++) {
    auto check_balance =
        make_check_balance_transaction(wallets[i], 10000 - ((i + 1) * 100));

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
