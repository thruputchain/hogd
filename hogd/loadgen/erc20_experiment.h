#pragma once

#include <cassert>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/loadgen/experiment_state.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "types.hh"

namespace hogd {

class ERC20ExperimentState : public ExperimentState {
public:
  ERC20ExperimentState(const ERC20PaymentsConfig &config) : config_(config) {}

  LoadgenStatus<LoadgenSuccessResult> generate_experiment();

  LoadgenStatus<LoadgenExtension> extend_experiment(uint64_t num_txs) override;

  ~ERC20ExperimentState() override = default;

private:
  ERC20PaymentsConfig config_;

  bool check_config_valid() const { return config_.num_accounts >= 2; }

  void initialize_wallets(SHA256Hash const &wallet_contract_hash);

  ContractAddress get_owner_address() const
  {
    // must be intialized by initialize_wallet_addresses
    assert(!wallets_.empty());
    return wallets_[0].address;
  }

  struct WalletInfo {
    Ed25519PublicKey public_key;
    Ed25519SecretKey secret_key;
    ContractAddress address;
    SHA256Hash initial_nonce;
  };

  // Owner wallet is wallet 0
  std::vector<WalletInfo> wallets_;
  ContractAddress erc20_address_;
};

} // namespace hogd
