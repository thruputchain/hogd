#include "hogd/loadgen/erc20_experiment.h"

#include <algorithm>
#include <cassert>
#include <future>
#include <limits>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "built_contracts/itc_erc20.h"
#include "built_contracts/simple_wallet.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/loadgen/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "loadgen.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {
SHA256Hash
make_nonce(uint64_t value)
{
  return sha256(value);
}

BlockHeight
make_init_mint_expiry_height(size_t mint_index)
{
  // We cannot have more than [kReplayCacheNoncesMaxLen] uniquifiers per wallet
  // for a given expiry height. In other words, for all txs that expire at some
  // height x, no more than [kReplayCacheNoncesMaxLen] txs can be from the same
  // wallet. Any excess txs will fail with [ExecResultTxNonDeterministicError].
  constexpr size_t kInitMintUniquifiersPerHeight = kReplayCacheNoncesMaxLen;
  return BlockHeight{1'000'000'000} +
         static_cast<BlockHeight>(mint_index / kInitMintUniquifiersPerHeight);
}

std::tuple<uint64_t, uint64_t>
make_amount(uint128_t amount)
{
  return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
}

std::pair<Ed25519PublicKey, Ed25519SecretKey>
make_wallet_keypair(SHA256Hash const &wallet_contract_hash, size_t wallet_index)
{
  SHA256Hash seed = sha256(x23::serialize(wallet_contract_hash,
                                          static_cast<uint64_t>(wallet_index)));
  return ed25519_keygen_from_seed(std::as_bytes(std::span(seed)));
}
} // namespace

void
ERC20ExperimentState::initialize_wallets(SHA256Hash const &wallet_contract_hash)
{
  wallets_.reserve(config_.num_addresses + 1);
  for (size_t i = 0; i < config_.num_addresses + 1; ++i) {
    auto keys = make_wallet_keypair(wallet_contract_hash, i);
    wallets_.push_back(WalletInfo{
        .public_key = keys.first,
        .secret_key = keys.second,
        .address = GenesisConfigurationLoader::calculate_deployed_address(
            wallet_contract_hash, make_nonce(i)),
        .initial_nonce = make_nonce(i),
    });
  }
}

x23::Async<LoadgenStatus<LoadgenSuccessResult>>
ERC20ExperimentState::generate_experiment()
{
  using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
  using wallet_meta = ContractTraitMeta<simple_wallet::SimpleWallet>;

  auto erc20_contract =
      erc20_meta::load_rv32();
  auto wallet_contract = wallet_meta::load_rv32();
  assert(erc20_contract);
  assert(wallet_contract);

  initialize_wallets(sha256(*wallet_contract));

  if (!check_config_valid()) {
    co_return LoadgenError{LoadgenRPCError::kLoadgenRPCErrorInvalidConfig};
  }

  const ContractAddress owner_address = get_owner_address();

  constexpr int128_t kWalletInitialHogsEpsilon = 100'000;
  const auto num_initial_allocations =
      static_cast<int128_t>(wallets_.size()) + 1;
  const int128_t wallet_initial_hogs =
      (GenesisConfigurationLoader::initial_hog_token_supply() /
       num_initial_allocations) -
      kWalletInitialHogsEpsilon;
  GenesisConfigurationBuilder builder;
  builder.add_initial_config(erc20_contract, make_nonce(0),
                             erc20_meta::entrypoint<u8"initialize">(),
                             erc20_meta::make_calldata<u8"initialize">(
                                 owner_address),
                             // The number of hogs each wallet starts with.
                             wallet_initial_hogs);
  erc20_address_ = GenesisConfigurationLoader::calculate_deployed_address(
      sha256(*erc20_contract), make_nonce(0));

  // initialize wallets
  for (auto const &wallet : wallets_) {
    // Initialize with an arbitrarily large amount, so that the wallets have
    // storage available to support their token balances. This will be changed
    // in the future, with whatever initialization procedures we use, but it
    // works for now.
    builder.add_initial_config(wallet_contract, wallet.initial_nonce,
                               wallet_meta::entrypoint<u8"initialize">(),
                               wallet_meta::make_calldata<u8"initialize">(
                                   wallet.public_key),
                               wallet_initial_hogs);
  }

  // make unfurl txs
  x23::Vec<SignedTransaction> unfurl_txs;
  unfurl_txs.append_range(builder.make_unfurl_genesis_transactions());

  // make mint txs
  x23::Vec<SignedTransaction> mint_txs;
  mint_txs.reserve(config_.num_addresses);
  for (size_t i = 1; i <= config_.num_addresses; ++i) {

    // has to match the deserialized objects in simple_wallet::do_action
    Calldata internal_calldata =
        x23::serialize(erc20_address_, erc20_meta::entrypoint<u8"mint">(),
                       GasAmount{1'000'000},
                       erc20_meta::make_calldata<u8"mint">(
                           wallets_[i].address,
                           make_amount(uint128_t{1'000'000'000})));

    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto calldata = wallet_meta::make_calldata<u8"do_action">(
        ed25519_sign(wallets_[0].secret_key, internal_calldata).value(),
        internal_calldata);
    // NOLINTEND(bugprone-unchecked-optional-access)

    auto tx = SignedTransaction{
        .tx =
            Transaction{
                .invocation =
                    Invocation{
                        .invoked_address = owner_address,
                        .entrypoint = wallet_meta::entrypoint<u8"do_action">(),
                        .gas_limit = work_gas_limit(),
                        .calldata = std::move(calldata),
                    },
                .uniquifier =
                    Uniquifier{.block_expiry_height =
                                   make_init_mint_expiry_height(i - 1),
                               .nonce = unfurl_txs.size() + i},
            },
    };
    mint_txs.push_back(tx);
  }

  x23::Vec<InitializationBlock> initialization_txs;
  initialization_txs.reserve(2);
  initialization_txs.emplace_back(std::move(unfurl_txs));
  initialization_txs.emplace_back(std::move(mint_txs));

  // Build result step by step
  auto genesis_config =
      builder.build_or_panic(StorageCostModel::default_cost_model());
  LoadgenSuccessResult result;
  result.genesis_configuration = std::move(genesis_config);
  result.initialization_txs = std::move(initialization_txs);
  co_return result;
}

x23::Async<LoadgenStatus<LoadgenExtension>>
ERC20ExperimentState::extend_experiment(uint64_t num_txs,
                                        BlockHeight expiry_height) const
{
  if (!check_config_valid()) {
    co_return LoadgenError{LoadgenRPCError::kLoadgenRPCErrorInvalidConfig};
  }

  x23::Vec<SignedTransaction> work_txs;
  work_txs.resize(num_txs);

  size_t nthread = WorkerPool::get_default_pool().worker_count();

  size_t batch_size = num_txs / nthread;
  if (num_txs % nthread != 0) {
    batch_size++;
  }

  std::vector<std::future<void>> futures;
  futures.reserve(nthread);
  // Wait for every submitted job before work_txs (captured by reference in the
  // jobs below) can be destroyed. Without this, an exception from submit()
  // partway through the loop would unwind the coroutine frame and free work_txs
  // while already-enqueued workers are still writing into it -> use-after-free.
  // Mirrors the drain guard in WorkerPool::for_each_parallel.
  Defer drain{[&futures] {
    for (auto &future : futures) {
      future.wait();
    }
  }};
  for (size_t i = 0; i < nthread; ++i) {
    size_t start = i * batch_size;
    size_t end = std::min<size_t>(start + batch_size, num_txs);
    if (i == nthread - 1) {
      assert(end == num_txs);
    }

    futures.emplace_back(WorkerPool::get_default_pool().submit(
        [this, &work_txs, start, end, expiry_height]() {
          std::random_device rd;
          std::mt19937 gen(rd());

          std::uniform_int_distribution<size_t> dist(1, config_.num_addresses);

          uint64_t base_nonce = std::uniform_int_distribution<uint64_t>(
              0, std::numeric_limits<uint64_t>::max())(gen);

          auto make_account_pair = [&]() -> std::pair<size_t, size_t> {
            auto sender = dist(gen);
            auto receiver = sender;
            while (receiver == sender) {
              receiver = dist(gen);
            }
            return std::make_pair(sender, receiver);
          };

          using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
          using wallet_meta = ContractTraitMeta<simple_wallet::SimpleWallet>;

          for (size_t j = start; j < end; ++j) {
            auto [sender, receiver] = make_account_pair();
            uint128_t amount = 10;

            // has to match the deserialized objects in simple_wallet::do_action
            static auto entrypoint_transfer =
                erc20_meta::entrypoint<u8"transfer">();
            Calldata internal_calldata =
                x23::serialize(erc20_address_, entrypoint_transfer,
                               GasAmount{1'000'000},
                               erc20_meta::make_calldata<u8"transfer">(
                                   wallets_[receiver].address,
                                   make_amount(amount)));

            static auto entrypoint_do_action =
                wallet_meta::entrypoint<u8"do_action">();
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            Invocation invocation = {
                .invoked_address = wallets_[sender].address,
                .entrypoint = entrypoint_do_action,
                .gas_limit = work_gas_limit(),
                .calldata = wallet_meta::make_calldata<u8"do_action">(
                    ed25519_sign(wallets_[sender].secret_key, internal_calldata)
                        .value(),
                    internal_calldata),
            };
            // NOLINTEND(bugprone-unchecked-optional-access)

            work_txs[j] = SignedTransaction{
                .tx =
                    Transaction{
                        .invocation = std::move(invocation),
                        .uniquifier =
                            Uniquifier{.block_expiry_height = expiry_height,
                                       .nonce = base_nonce + j},
                    },
            };
          }
        }));
  }

  // Fill workers must finish before t_to_parallel_batch reads work_txs below.
  drain.reset();

  LoadgenExtension ret;
  co_await t_to_parallel_batch(x23::span<const SignedTransaction>(work_txs),
                               ret.work_txs);
  co_return ret;
}

} // namespace hogd
