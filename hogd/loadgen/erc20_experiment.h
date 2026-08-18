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
  // num_virtual_addresses is a server-side knob rather than part of [config],
  // so that raising it needs no change to the loadgen RPC. See
  // make_virtual_address.
  // filler_instances deploys that many storage_filler contracts at genesis and
  // sends every filler_period'th work transaction to one of them, writing
  // filler_items items of filler_item_bytes each. It exists to make state grow
  // with the workload instead of with the transaction count: an erc20 transfer
  // adds ~392 bytes, so filling a large cache by transfers alone takes hours.
  // Many instances rather than one because storage items are keyed by the
  // *contract's* address, so a single instance would pin all of that state to
  // one shard. Zero instances disables it, which is the normal path.
  ERC20ExperimentState(const ERC20PaymentsConfig &config, bool use_rv32 = false,
                       uint64_t num_virtual_addresses = 0,
                       uint32_t filler_instances = 0,
                       uint32_t filler_items = 0,
                       uint32_t filler_item_bytes = 0,
                       uint32_t filler_period = 1)
    : config_(config), use_rv32_(use_rv32),
      num_virtual_addresses_(num_virtual_addresses),
      filler_instances_(filler_instances), filler_items_(filler_items),
      filler_item_bytes_(filler_item_bytes),
      filler_period_(filler_period ? filler_period : 1)
  {}

  x23::Async<LoadgenStatus<LoadgenSuccessResult>> generate_experiment();

  x23::Async<LoadgenStatus<LoadgenExtension>>
  extend_experiment(uint64_t num_txs, BlockHeight expiry_height) const override;

  ~ERC20ExperimentState() override = default;

private:
  ERC20PaymentsConfig config_;
  bool use_rv32_ = false;
  uint64_t num_virtual_addresses_ = 0;
  uint32_t filler_instances_ = 0;
  uint32_t filler_items_ = 0;
  uint32_t filler_item_bytes_ = 0;
  uint32_t filler_period_ = 1;

  bool check_config_valid() const { return config_.num_addresses >= 2; }

  // Outer gas limit for a wallet do_action tx. rv32 meters one gas per
  // instruction, so the same do_action + cross-called erc20 transfer/mint costs
  // ~2.4x the wasm gas (measured ~29k for a transfer vs ~12k on wasm); the rv32
  // envelope is sized to cover that plus first-touch storage on the mint path.
  static GasAmount work_gas_limit() { return GasAmount{100'000}; }

  // A filler transaction writes filler_items_ storage items, and each write
  // costs gas, so the transfer-sized limit above caps it at about four items.
  // Measured on wasm: 8,243 gas for one 1 KiB write and 15,335 for four, i.e.
  // ~2,364 marginal gas an item. Budget that plus the transfer-sized headroom,
  // and scale by the rv32/wasm ratio the way work_gas_limit does.
  GasAmount filler_gas_limit() const
  {
    const uint64_t marginal = 2'500ULL * (filler_items_ ? filler_items_ : 1);
    const uint64_t base = use_rv32_ ? 100'000ULL : 20'000ULL;
    return GasAmount{base + (use_rv32_ ? 3ULL : 1ULL) * marginal};
  }

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
  std::vector<ContractAddress> filler_addresses_;
};

} // namespace hogd
