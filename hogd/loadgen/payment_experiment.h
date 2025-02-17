#pragma once

#include <map>
#include <memory>
#include <random>
#include <vector>

#include "contract.hh"
#include "hogd/base/types.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class PaymentExperiment {
public:
  PaymentExperiment(uint32_t num_accounts);

  using amount_t = int128_t;

  // Relies on having the hardcoded deployer contract started at genesis.

  std::vector<std::vector<ProposedTransaction>>
  produce_initialization_transactions();

  // Does not validate whether balances are sufficient.
  std::vector<ProposedTransaction>
  make_random_pairwise_payments(uint32_t count);

private:
  struct Account {
    uint32_t account_index_;
    ContractAddress wallet_address_;
    Ed25519PublicKey public_key_;
    Ed25519SecretKey secret_key_;

    SignedTransaction make_deploy_wallet_transaction(
        SHA256Hash const &wallet_contract_hash,
        std::shared_ptr<const Contract> const &wallet_contract,
        ContractAddress const &erc20_address) const;

    ProposedTransaction make_check_balance_transaction(amount_t amount) const;
  };

  void generate_accounts();

  // Workflow: Deploy ERC20 and owner, then deploy wallets.
  std::vector<ProposedTransaction> make_deploy_erc20_and_owner_transactions();
  std::vector<ProposedTransaction> make_deploy_wallet_transactions();
  std::vector<ProposedTransaction> make_mint_transactions();
  std::vector<ProposedTransaction> make_check_balance_transactions();

  ProposedTransaction make_mint_transaction(amount_t amount,
                                            Account const &to) const;

  static ProposedTransaction make_pairwise_payment(Account const &from,
                                                   Account const &to,
                                                   amount_t amount);

  std::minstd_rand gen_;

  SHA256Hash wallet_contract_hash_;
  SHA256Hash erc20_contract_hash_;
  std::shared_ptr<const Contract> wallet_contract_;
  std::shared_ptr<const Contract> erc20_contract_;

  ContractAddress erc20_address_;

  const uint32_t num_accounts_;
  // TODO(geoff): other distribution params

  std::map<uint64_t, Account> accounts_;
  Account erc20_owner_;
};
} // namespace hogd
