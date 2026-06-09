#pragma once

#include <cassert>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/loadgen/experiment_state.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class ERC20ExperimentState : public ExperimentState {
public:
  // use_rv32 selects which compiled artifacts back the erc20 and wallet
  // contracts: the RV32 (.rv32a, kRv32 runtime) builds when true, the wasm
  // builds otherwise. Everything else (calldata, tx shape) is identical.
  ERC20ExperimentState(const ERC20PaymentsConfig &config, bool use_rv32 = false)
    : config_(config), use_rv32_(use_rv32)
  {}

  x23::Async<LoadgenStatus<LoadgenSuccessResult>> generate_experiment();

  x23::Async<LoadgenStatus<LoadgenExtension>>
  extend_experiment(uint64_t num_txs, BlockHeight expiry_height) const override;

  ~ERC20ExperimentState() override = default;

private:
  ERC20PaymentsConfig config_;
  bool use_rv32_ = false;

  bool check_config_valid() const { return config_.num_addresses >= 2; }

  // Outer gas limit for a wallet do_action tx. The rv32 builds interpret many
  // more instructions for the same work than the wasm builds, so they need a
  // larger envelope to cover do_action + the cross-called erc20 transfer/mint.
  GasAmount work_gas_limit() const
  {
    return use_rv32_ ? GasAmount{2'000'000} : GasAmount{20'000};
  }

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
