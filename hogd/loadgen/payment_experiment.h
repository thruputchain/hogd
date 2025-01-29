#pragma once

#include <cstdint>
#include <map>

#include "transaction.hh"
#include "types.hh"

namespace hogd {

class PaymentExperiment {
public:
  PaymentExperiment(uint32_t num_accounts);

  // Relies on having the hardcoded deployer contract started at genesis.

  std::vector<std::vector<ProposedTransaction>>
  produce_initialization_transactions();

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
  };

  void generate_accounts();

  // Workflow: Deploy ERC20 and owner, then deploy wallets.
  std::vector<ProposedTransaction> make_deploy_erc20_and_owner_transactions();

  std::vector<ProposedTransaction> make_deploy_wallet_transactions();

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
