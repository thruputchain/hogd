#include "hogd/hogexec/runtime/erc20_test_utils.h"

#include <cstdint>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "storage_interface.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd ::test ::erc20 {

SignedTransaction
make_deploy_wallet_transaction(SHA256Hash const &h_wallet,
                               std::shared_ptr<const Contract> const &c_wallet,
                               uint64_t nonce, Ed25519PublicKey const &pk,
                               ContractAddress const &erc20_addr)
{

  SignedTransaction wallet_stx;
  wallet_stx.tx.invocation.invoked_address =
      test::RuntimeTestFramework::kDeployerAddress;
  wallet_stx.tx.invocation.method_name = 1;
  wallet_stx.tx.invocation.gas_limit = UINT64_MAX;
  wallet_stx.tx.invocation.calldata =
      test::make_calldata(h_wallet, test::get_sdk_name("initialize"), pk,
                          erc20_addr);
  wallet_stx.tx.nonce = nonce;
  wallet_stx.tx.contracts_to_deploy.push_back(*c_wallet);

  return wallet_stx;
}

SignedTransaction
make_deploy_erc20_noinit(SHA256Hash const &h_erc20,
                         std::shared_ptr<const Contract> const &c_erc20)
{

  SignedTransaction stx;
  stx.tx.invocation.invoked_address =
      test::RuntimeTestFramework::kDeployerAddress;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata(h_erc20);
  stx.tx.contracts_to_deploy.push_back(*c_erc20);
  stx.tx.nonce = 0;

  return stx;
}

SignedTransaction
make_initialize_erc20(ContractAddress const &owner,
                      ContractAddress const &erc20_address)
{

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = erc20_address;
  stx.tx.invocation.method_name = test::get_sdk_name("initialize");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata(owner);
  stx.tx.nonce = 0;

  return stx;
}

SignedTransaction
make_mint_transaction(ContractAddress const &manager_wallet,
                      ContractAddress const &to, int64_t amount)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = manager_wallet;
  stx.tx.invocation.method_name = test::get_sdk_name("mint");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata(to, amount);
  stx.tx.nonce = 0;

  return stx;
}

SignedTransaction
make_check_balance_transaction(ContractAddress const &wallet, int64_t value)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = wallet;
  stx.tx.invocation.method_name = test::get_sdk_name("test_expect_balance");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata(value);
  stx.tx.nonce = 0;

  return stx;
}

SignedTransaction
make_transfer_transaction(ContractAddress const &sender,
                          ContractAddress const &to, int64_t amount)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = sender;
  stx.tx.invocation.method_name = test::get_sdk_name("send_payment");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata(to, amount);
  stx.tx.nonce = 0;

  return stx;
}

std::optional<ContractAddress>
deploy_erc20_noinit(RuntimeTestFramework &runtime_framework)
{
  auto [h_erc20, c_erc20] = test::load_built_rs_contract("erc20.wasm");

  auto deploy_erc20_tx = make_deploy_erc20_noinit(h_erc20, c_erc20);

  SystemCallHandler handler(deploy_erc20_tx,
                            *runtime_framework.snapshot_cache_);

  if (!handler.root_invoke().result) {
    return std::nullopt;
  }

  if (handler.try_reserve_transaction(runtime_framework.get_write_client()) !=
      StorageRPCError::kNone) {
    return std::nullopt;
  }

  handler.commit_transaction(runtime_framework.get_write_client());

  return SystemCallHandler::static_get_deploy_address(
      test::RuntimeTestFramework::kDeployerAddress,
      handler.get_source_tx_hash(), h_erc20);
}

std::optional<ContractAddress>
deploy_and_initialize_wallet(RuntimeTestFramework &runtime_framework,
                             ContractAddress const &erc20_address,
                             uint64_t nonce)
{
  auto [h_wallet, c_wallet] =
      test::load_built_rs_contract("simple_wallet.wasm");

  auto deploy_manager_tx =
      make_deploy_wallet_transaction(h_wallet, c_wallet, nonce,
                                     Ed25519PublicKey{}, erc20_address);

  SystemCallHandler handler(deploy_manager_tx,
                            *runtime_framework.snapshot_cache_);

  if (!handler.root_invoke().result) {
    return std::nullopt;
  }

  if (handler.try_reserve_transaction(runtime_framework.get_write_client()) !=
      StorageRPCError::kNone) {
    return std::nullopt;
  }
  handler.commit_transaction(runtime_framework.get_write_client());

  return SystemCallHandler::static_get_deploy_address(
      test::RuntimeTestFramework::kDeployerAddress,
      handler.get_source_tx_hash(), h_wallet);
}

// Initialize the erc20 (set the owner to be the manager wallet)
bool
initialize_erc20(RuntimeTestFramework &runtime_framework,
                 ContractAddress const &manager_address,
                 ContractAddress const &erc20_address)
{
  auto init_erc20_tx = make_initialize_erc20(manager_address, erc20_address);

  SystemCallHandler handler(init_erc20_tx, *runtime_framework.snapshot_cache_);

  if (!handler.root_invoke().result) {
    return false;
  }

  if (handler.try_reserve_transaction(runtime_framework.get_write_client()) !=
      StorageRPCError::kNone) {
    return false;
  }

  handler.commit_transaction(runtime_framework.get_write_client());
  return true;
}

bool
mint_to(RuntimeTestFramework &runtime_framework,
        ContractAddress const &manager_address, ContractAddress const &wallet,
        int64_t amount)
{
  auto mint_tx = make_mint_transaction(manager_address, wallet, amount);

  SystemCallHandler handler(mint_tx, *runtime_framework.snapshot_cache_);

  if (!handler.root_invoke().result) {
    return false;
  }

  if (handler.try_reserve_transaction(runtime_framework.get_write_client()) !=
      StorageRPCError::kNone) {
    return false;
  }

  handler.commit_transaction(runtime_framework.get_write_client());

  return true;
}

bool
send_payment(RuntimeTestFramework &runtime_framework,
             ContractAddress const &from, ContractAddress const &to,
             int64_t amount)
{
  auto payment = make_transfer_transaction(from, to, amount);

  SystemCallHandler handler(payment, *runtime_framework.snapshot_cache_);
  if (!handler.root_invoke().result) {
    return false;
  }

  if (handler.try_reserve_transaction(runtime_framework.get_write_client()) !=
      StorageRPCError::kNone) {
    return false;
  }

  handler.commit_transaction(runtime_framework.get_write_client());

  return true;
}

} // namespace hogd::test::erc20
