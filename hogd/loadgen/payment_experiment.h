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
    ContractAddress wallet_address_;
    Ed25519PublicKey public_key_;
    Ed25519SecretKey secret_key_;
  };

  void generate_accounts();

  std::vector<ProposedTransaction> make_deploy_wallet_transactions();
  std::vector<ProposedTransaction> make_deploy_erc20_transaction();

  const uint32_t num_accounts_;
  // TODO(geoff): other distribution params

  std::map<uint64_t, Account> accounts_;
};
} // namespace hogd
