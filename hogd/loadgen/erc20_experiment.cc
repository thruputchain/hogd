#include "hogd/loadgen/erc20_experiment.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include "built_contracts/itc_erc20.h"
#include "built_contracts/simple_wallet.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_block.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {
SHA256Hash
make_nonce(uint64_t value)
{
  return sha256(value);
}
} // namespace

void
ERC20ExperimentState::initialize_wallets(SHA256Hash const &wallet_contract_hash)
{
  wallets_.reserve(config_.num_accounts + 1);
  for (size_t i = 0; i < config_.num_accounts + 1; ++i) {
    auto keys = ed25519_keygen();
    wallets_.push_back(WalletInfo{
        .public_key = keys.first,
        .secret_key = keys.second,
        .address =
            GenesisBlockLoader::calculate_deployed_address(wallet_contract_hash,
                                                           make_nonce(i)),
        .initial_nonce = make_nonce(i),
    });
  }
}

LoadgenStatus<LoadgenSuccessResult>
ERC20ExperimentState::generate_experiment()
{
  using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
  using wallet_meta = ContractTraitMeta<simple_wallet::SimpleWallet>;

  auto erc20_contract = erc20_meta::load_wasm();
  auto wallet_contract = wallet_meta::load_wasm();
  assert(erc20_contract);
  assert(wallet_contract);

  initialize_wallets(sha256(*wallet_contract));

  if (!check_config_valid()) {
    return {LoadgenError{LoadgenRPCError::kLoadgenRPCErrorInvalidConfig}};
  }

  const ContractAddress owner_address = get_owner_address();

  GenesisBlockBuilder builder;
  builder.add_initial_config(erc20_contract, make_nonce(0),
                             erc20_meta::entrypoint<"initialize">(),
                             erc20_meta::make_calldata<"initialize">(
                                 owner_address),
                             int128_t{1'000'000} * config_.num_accounts);
  erc20_address_ =
      GenesisBlockLoader::calculate_deployed_address(sha256(*erc20_contract),
                                                     make_nonce(0));

  // initialize wallets
  for (auto const &wallet : wallets_) {
    // Initialize with an arbitrarily large amount, so that the wallets have
    // storage available to support their token balances. This will be changed
    // in the future, with whatever initialization procedures we use, but it
    // works for now.
    builder.add_initial_config(wallet_contract, wallet.initial_nonce,
                               wallet_meta::entrypoint<"initialize">(),
                               wallet_meta::make_calldata<"initialize">(
                                   wallet.public_key),
                               1'000'000'000);
  }

  // make mint txs
  x23::Vec<SignedTransaction> mint_txs;
  mint_txs.reserve(config_.num_accounts);
  for (size_t i = 1; i <= config_.num_accounts; ++i) {

    // has to match the deserialized objects in simple_wallet::do_action
    Calldata internal_calldata = x23::serialize(
        erc20_address_, erc20_meta::entrypoint<"mint">(), GasAmount{1'000'000},
        erc20_meta::make_calldata<"mint">(wallets_[i].address,
                                          to_xdr(uint128_t{1'000'000'000})));

    auto tx = SignedTransaction{
        .tx =
            Transaction{
                .invocation =
                    {
                        .invoked_address = owner_address,
                        .entrypoint = wallet_meta::entrypoint<"do_action">(),
                        .gas_limit = GasAmount{1'000'000},
                        .calldata = wallet_meta::make_calldata<"do_action">(
                            ed25519_sign(wallets_[0].secret_key,
                                         internal_calldata),
                            internal_calldata),
                    },
            },
    };
    mint_txs.push_back(tx);
  }

  x23::Vec<SignedTransaction> unfurl_txs;
  unfurl_txs.append_range(builder.make_unfurl_genesis_transactions());

  return {LoadgenSuccessResult{
      .genesis_block = builder.build(),
      .initialization_txs = {std::move(unfurl_txs), std::move(mint_txs)},
  }};
}

LoadgenStatus<LoadgenExtension>
ERC20ExperimentState::extend_experiment(uint64_t num_txs)
{
  if (!check_config_valid()) {
    return {LoadgenError{LoadgenRPCError::kLoadgenRPCErrorInvalidConfig}};
  }

  const size_t batch_size =
      std::max(static_cast<size_t>(10),
               static_cast<size_t>(num_txs) /
                   (4 * WorkerPool::get_default_pool().worker_count()));

  x23::Vec<SignedTransaction> work_txs;
  work_txs.resize(num_txs);

  // Create a vector of indices to process in parallel
  std::vector<size_t> indices(num_txs);
  // NOLINTNEXTLINE(modernize-use-ranges)
  std::iota(indices.begin(), indices.end(), 0);

  WorkerPool::get_default_pool().for_each_parallel(
      indices,
      [this, &work_txs](size_t i) {
        // Each thread needs its own random number generator to avoid contention
        thread_local static std::random_device rd;
        thread_local static std::mt19937 gen(rd());

        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
        using wallet_meta = ContractTraitMeta<simple_wallet::SimpleWallet>;

        std::uniform_int_distribution<size_t> dist(1, config_.num_accounts);

        auto make_account_pair = [&]() {
          auto sender = dist(gen);
          auto receiver = sender;
          while (receiver == sender) {
            receiver = dist(gen);
          }
          return std::make_pair(sender, receiver);
        };

        auto [sender, receiver] = make_account_pair();
        uint128_t amount = 10;

        // has to match the deserialized objects in simple_wallet::do_action
        Calldata internal_calldata =
            x23::serialize(erc20_address_, erc20_meta::entrypoint<"transfer">(),
                           GasAmount{1'000'000},
                           erc20_meta::make_calldata<"transfer">(
                               wallets_[receiver].address, to_xdr(amount)));

        auto tx = SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        {
                            .invoked_address = wallets_[sender].address,
                            .entrypoint =
                                wallet_meta::entrypoint<"do_action">(),
                            .gas_limit = GasAmount{1'000'000},
                            .calldata = wallet_meta::make_calldata<"do_action">(
                                ed25519_sign(wallets_[sender].secret_key,
                                             internal_calldata),
                                internal_calldata),
                        },
                },
        };

        work_txs[i] = std::move(tx);
      },
      batch_size);

  return {LoadgenExtension{
      .work_txs = std::move(work_txs),
  }};
}

} // namespace hogd
