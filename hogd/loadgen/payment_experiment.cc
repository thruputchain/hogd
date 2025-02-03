#include "hogd/loadgen/payment_experiment.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/shared_util/load_contracts.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

SHA256Hash
compute_deploy_nonce(uint32_t account_index)
{
  return sha256(account_index);
}

SHA256Hash
compute_deploy_address(uint32_t account_index, SHA256Hash const &contract_hash)
{
  constexpr static ContractAddress kDeployerAddress = ContractAddress{};
  return sha256(
      ContractDeploymentHashPreimage{.deployer_address = kDeployerAddress,
                                     .nonce =
                                         compute_deploy_nonce(account_index),
                                     .contract_hash = contract_hash});
}

} // namespace

PaymentExperiment::PaymentExperiment(uint32_t num_accounts)
  : num_accounts_(num_accounts)
{
  auto [wallet_hash, wallet_contract] =
      load_built_rs_contract("simple_wallet.wasm");
  auto [erc20_hash, erc20_contract] = load_built_rs_contract("erc20.wasm");

  wallet_contract_hash_ = wallet_hash;
  erc20_contract_hash_ = erc20_hash;
  wallet_contract_ = wallet_contract;
  erc20_contract_ = erc20_contract;

  generate_accounts();

  erc20_address_ = compute_deploy_address(0, erc20_contract_hash_);
}

void
PaymentExperiment::generate_accounts()
{
  // TODO(geoff): this is probably very slow.  Can be parallelized.
  for (uint32_t i = 1; i <= num_accounts_; ++i) {
    auto [public_key, secret_key] = deterministic_keypair(i);
    ContractAddress wallet_address =
        compute_deploy_address(i, wallet_contract_hash_);
    accounts_.emplace(i, Account{.account_index_ = i,
                                 .wallet_address_ = wallet_address,
                                 .public_key_ = public_key,
                                 .secret_key_ = secret_key});
  }
  auto [owner_public_key, owner_secret_key] = deterministic_keypair(0);
  erc20_owner_ = Account{.account_index_ = 0,
                         .wallet_address_ =
                             compute_deploy_address(0, wallet_contract_hash_),
                         .public_key_ = owner_public_key,
                         .secret_key_ = owner_secret_key};
}

SignedTransaction
PaymentExperiment::Account::make_deploy_wallet_transaction(
    SHA256Hash const &wallet_contract_hash,
    std::shared_ptr<const Contract> const &wallet_contract,
    ContractAddress const &erc20_address) const
{
  return genesis_deployer_deploy_and_invoke(
      compute_deploy_nonce(account_index_), wallet_contract_hash,
      wallet_contract, compute_sdk_name("initialize"),
      make_calldata(public_key_, erc20_address));
}

std::vector<ProposedTransaction>
PaymentExperiment::make_deploy_wallet_transactions()
{
  std::vector<ProposedTransaction> out;
  for (auto const &[account_index, account] : accounts_) {
    // wallet contract is already created, so we don't need to create it again.
    SignedTransaction stx =
        account.make_deploy_wallet_transaction(wallet_contract_hash_, nullptr,
                                               erc20_address_);
    out.push_back(ProposedTransaction{.signed_tx = stx});
  }
  return out;
}

std::vector<ProposedTransaction>
PaymentExperiment::make_deploy_erc20_and_owner_transactions()
{
  auto owner_tx = ProposedTransaction{
      .signed_tx = erc20_owner_.make_deploy_wallet_transaction(
          wallet_contract_hash_, wallet_contract_, erc20_address_)};

  auto erc20_tx =
      ProposedTransaction{.signed_tx = genesis_deployer_deploy_and_invoke(
                              compute_deploy_nonce(0), erc20_contract_hash_,
                              erc20_contract_, compute_sdk_name("initialize"),
                              make_calldata(erc20_owner_.wallet_address_))};

  return {owner_tx, erc20_tx};
}

std::vector<ProposedTransaction>
PaymentExperiment::make_mint_transactions()
{
  std::vector<ProposedTransaction> out;
  out.reserve(accounts_.size());
  for (auto const &[account_index, account] : accounts_) {
    out.push_back(make_mint_transaction(1'000'000, account));
  }
  return out;
}

std::vector<ProposedTransaction>
PaymentExperiment::make_check_balance_transactions()
{
  std::vector<ProposedTransaction> out;
  out.reserve(accounts_.size());
  for (auto const &[account_index, account] : accounts_) {
    out.push_back(account.make_check_balance_transaction(1'000'000));
  }
  return out;
}

std::vector<std::vector<ProposedTransaction>>
PaymentExperiment::produce_initialization_transactions()
{
  return {make_deploy_erc20_and_owner_transactions(),
          make_deploy_wallet_transactions(), make_mint_transactions(),
          make_check_balance_transactions()};
}

ProposedTransaction
PaymentExperiment::make_mint_transaction(int128_t amount,
                                         Account const &to) const
{
  return ProposedTransaction{
      .signed_tx = SignedTransaction{
          .tx = Transaction{
              .invocation =
                  Invocation{.invoked_address = erc20_owner_.wallet_address_,
                             .method = compute_sdk_name("mint"),
                             .gas_limit = UINT64_MAX,
                             .calldata =
                                 make_calldata(to.wallet_address_, amount)}}}};
}

ProposedTransaction
PaymentExperiment::Account::make_check_balance_transaction(
    int128_t amount) const
{
  return ProposedTransaction{
      .signed_tx = SignedTransaction{
          .tx = Transaction{
              .invocation =
                  Invocation{.invoked_address = wallet_address_,
                             .method = compute_sdk_name("test_expect_balance"),
                             .gas_limit = UINT64_MAX,
                             .calldata = make_calldata(amount)}}}};
}

} // namespace hogd
