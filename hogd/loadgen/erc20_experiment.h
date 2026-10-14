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
  ERC20ExperimentState(const ERC20PaymentsConfig &config) : config_(config) {}

  x23::Async<LoadgenStatus<LoadgenSuccessResult>> generate_experiment();

  x23::Async<LoadgenStatus<LoadgenExtension>>
  extend_experiment(uint64_t num_txs, BlockHeight expiry_height) const override;

  ~ERC20ExperimentState() override = default;

private:
  ERC20PaymentsConfig config_;

  bool check_config_valid() const { return config_.num_addresses >= 2; }

  // Outer gas limit for a wallet do_action tx. rv32 meters one gas per
  // instruction, so the same do_action + cross-called erc20 transfer/mint costs
  // ~2.4x what the former wasm build did (measured ~29k for a transfer vs
  // ~12k on wasm).
  //
  // The envelope is dominated by storage now that kDBRPCCost prices the load
  // from the database: a transfer measures 134,716, of which three cold keys
  // are ~112k. Sized with room for the mint path's extra touch rather than
  // trimmed to the measurement.
  static GasAmount work_gas_limit() { return GasAmount{250'000}; }

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

  // Owner wallet is wallet 0
  std::vector<WalletInfo> wallets_;
  ContractAddress erc20_address_;
};

} // namespace hogd
