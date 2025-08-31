#include "hogd/loadgen/erc20_experiment.h"

#include <cassert>
#include <random>
#include <utility>
#include <vector>

#include "built_contracts/itc_erc20.h"
#include "built_contracts/simple_wallet.h"
#include "hogd/base/types.h"
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

LoadgenStatus<LoadgenSuccessResult>
generate_erc20_payments_experiment(const ERC20PaymentsConfig &config)
{
  using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
  using wallet_meta = ContractTraitMeta<simple_wallet::SimpleWallet>;

  auto erc20_contract = erc20_meta::load_wasm();
  auto wallet_contract = wallet_meta::load_wasm();
  assert(erc20_contract);
  assert(wallet_contract);

  const ContractAddress owner_address =
      GenesisBlockLoader::calculate_deployed_address(sha256(*wallet_contract),
                                                     make_nonce(0));

  GenesisBlockBuilder builder;
  builder.add_initial_config(erc20_contract, make_nonce(0),
                             erc20_meta::entrypoint<"initialize">(),
                             erc20_meta::make_calldata<"initialize">(
                                 owner_address),
                             int128_t{1'000'000} * config.num_accounts);
  const auto erc20_address =
      GenesisBlockLoader::calculate_deployed_address(sha256(*erc20_contract),
                                                     make_nonce(0));

  // initialize public keys
  // Owner wallet is wallet 0
  std::vector<std::pair<Ed25519PublicKey, Ed25519SecretKey>> keys;
  keys.reserve(config.num_accounts + 1);
  for (size_t i = 0; i < config.num_accounts + 1; ++i) {
    keys.push_back(ed25519_keygen());
  }

  // initialize wallets
  std::vector<ContractAddress> wallet_addresses;
  wallet_addresses.reserve(config.num_accounts + 1);
  for (size_t i = 0; i < config.num_accounts + 1; ++i) {
    auto nonce = make_nonce(i);
    // Initialize with an arbitrarily large amount, so that the wallets have
    // storage available to support their token balances. This will be changed
    // in the future, with whatever initialization procedures we use, but it
    // works for now.
    builder.add_initial_config(
        wallet_contract, nonce, wallet_meta::entrypoint<"initialize">(),
        wallet_meta::make_calldata<"initialize">(keys[i].first), 1'000'000'000);
    wallet_addresses.push_back(
        GenesisBlockLoader::calculate_deployed_address(sha256(*wallet_contract),
                                                       nonce));
  }

  // make mint txs
  x23::Vec<SignedTransaction> mint_txs;
  mint_txs.reserve(config.num_accounts);
  for (size_t i = 1; i <= config.num_accounts; ++i) {

    // has to match the deserialized objects in simple_wallet::do_action
    Calldata internal_calldata = x23::serialize(
        erc20_address, erc20_meta::entrypoint<"mint">(), GasAmount{1'000'000},
        erc20_meta::make_calldata<"mint">(wallet_addresses[i],
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
                            ed25519_sign(keys[0].second, internal_calldata),
                            internal_calldata),
                    },
            },
    };
    mint_txs.push_back(tx);
  }

  // make work txs
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dist(1, config.num_accounts);

  auto make_account_pair = [&]() {
    auto i = dist(gen);
    auto j = i;
    while (j == i) {
      j = dist(gen);
    }
    return std::make_pair(i, j);
  };

  x23::Vec<SignedTransaction> work_txs;
  work_txs.reserve(config.num_txs);
  for (size_t i = 0; i < config.num_txs; ++i) {
    auto [sender, receiver] = make_account_pair();

    uint128_t amount = 10;

    // has to match the deserialized objects in simple_wallet::do_action
    Calldata internal_calldata =
        x23::serialize(erc20_address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{1'000'000},
                       erc20_meta::make_calldata<"transfer">(
                           wallet_addresses[receiver], to_xdr(amount)));

    auto tx = SignedTransaction{
        .tx =
            Transaction{
                .invocation =
                    {
                        .invoked_address = wallet_addresses[sender],
                        .entrypoint = wallet_meta::entrypoint<"do_action">(),
                        .gas_limit = GasAmount{1'000'000},
                        .calldata = wallet_meta::make_calldata<"do_action">(
                            ed25519_sign(keys[sender].second,
                                         internal_calldata),
                            internal_calldata),
                    },
            },
    };

    work_txs.push_back(tx);
  }

  x23::Vec<SignedTransaction> unfurl_txs;
  unfurl_txs.append_range(builder.make_unfurl_genesis_transactions());

  return {LoadgenSuccessResult{
      .genesis_block = builder.build(),
      .initialization_txs = {std::move(unfurl_txs), std::move(mint_txs)},
      .work_txs = std::move(work_txs),
  }};
}

} // namespace hogd
