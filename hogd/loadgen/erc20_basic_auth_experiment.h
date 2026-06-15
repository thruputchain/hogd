#pragma once

#include <cassert>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/loadgen/experiment_state.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class ERC20BasicAuthExperimentState : public ExperimentState {
public:
  ERC20BasicAuthExperimentState(const ERC20PaymentsConfig &config)
    : config_(config)
  {}

  x23::Async<LoadgenStatus<LoadgenSuccessResult>> generate_experiment();

  x23::Async<LoadgenStatus<LoadgenExtension>>
  extend_experiment(uint64_t num_txs, BlockHeight expiry_height) const override;

  ~ERC20BasicAuthExperimentState() override = default;

private:
  ERC20PaymentsConfig config_;

  bool check_config_valid() const { return config_.num_addresses >= 2; }

  void initialize_wallets(SHA256Hash const &wallet_contract_hash);

  ContractAddress get_owner_address() const
  {
    // must be initialized by initialize_wallet_addresses
    assert(!wallets_.empty());
    return wallets_[0].address;
  }

  struct WalletInfo {
    Ed25519PublicKey public_key;
    Ed25519SecretKey secret_key;
    ContractAddress address;
    SHA256Hash initial_nonce;
  };

  void add_signature(SignedTransaction &tx, size_t wallet_index) const;

  // Owner wallet is wallet 0
  std::vector<WalletInfo> wallets_;
  ContractAddress erc20_address_;
};

} // namespace hogd
