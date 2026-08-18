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
  // num_virtual_addresses is a server-side knob rather than part of [config],
  // so that raising it needs no change to the loadgen RPC. See
  // make_virtual_address.
  // filler_* mirror ERC20ExperimentState's knobs. They live here too because
  // this is the experiment chuckd actually asks for
  // (KExperimentTypeERC20BasicAuthPayments in chuckd's sub/loadgen/loadgen.go),
  // so a filler wired only into ERC20ExperimentState never runs on the cluster.
  ERC20BasicAuthExperimentState(const ERC20PaymentsConfig &config,
                                uint64_t num_virtual_addresses = 0,
                                uint32_t filler_instances = 0,
                                uint32_t filler_items = 0,
                                uint32_t filler_item_bytes = 0,
                                uint32_t filler_period = 1,
                                uint64_t filler_keyspace = 0)
    : config_(config), num_virtual_addresses_(num_virtual_addresses),
      filler_instances_(filler_instances), filler_items_(filler_items),
      filler_item_bytes_(filler_item_bytes),
      filler_period_(filler_period ? filler_period : 1),
      filler_keyspace_(filler_keyspace)
  {}

  x23::Async<LoadgenStatus<LoadgenSuccessResult>> generate_experiment();

  x23::Async<LoadgenStatus<LoadgenExtension>>
  extend_experiment(uint64_t num_txs, BlockHeight expiry_height) const override;

  ~ERC20BasicAuthExperimentState() override = default;

private:
  ERC20PaymentsConfig config_;
  uint64_t num_virtual_addresses_ = 0;
  uint32_t filler_instances_ = 0;
  uint32_t filler_items_ = 0;
  uint32_t filler_item_bytes_ = 0;
  uint32_t filler_period_ = 1;
  // Distinct key groups the filler cycles through. Zero means every transaction
  // writes fresh keys, so writes are always inserts and live state grows without
  // bound. A nonzero value makes transactions revisit keys, so writes become
  // overwrites and live state is capped at keyspace x items x item_bytes. The
  // difference tells us whether the hogdb data directory grows with distinct
  // keys or with the number of writes -- i.e. whether a 105 GB directory means
  // 105 GB of state or ~10 GB of state plus write amplification.
  uint64_t filler_keyspace_ = 0;

  // Outer gas for a filler transaction: measured on wasm at 8,243 gas for one
  // 1 KiB write and 15,335 for four, ~2,364 marginal an item, against the
  // 20,000 a transfer gets.
  GasAmount filler_gas_limit() const
  {
    return GasAmount{20'000 +
                     2'500ULL * (filler_items_ ? filler_items_ : 1)};
  }

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
  std::vector<ContractAddress> filler_addresses_;
};

} // namespace hogd
