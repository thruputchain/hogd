#include "hogd/devnode/bootstrap_accounts.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/dev_wallet.h"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"

namespace hogd {

namespace {

SHA256Hash
derive_labeled_hash(std::string_view seed, std::string_view label)
{
  SHA256Hasher hasher;
  hasher.update(std::as_bytes(std::span(seed.data(), seed.size())));
  constexpr char kSeparator = ':';
  hasher.update(std::as_bytes(std::span(&kSeparator, 1)));
  hasher.update(std::as_bytes(std::span(label.data(), label.size())));
  return hasher.finalize();
}

SHA256Hash
derive_account_secret_seed(std::string_view seed, std::uint32_t index)
{
  return derive_labeled_hash(seed, std::format("account-{}", index));
}

SHA256Hash
derive_wallet_nonce(std::string_view seed, std::uint32_t index)
{
  return derive_labeled_hash(seed, std::format("wallet-{}", index));
}

BootstrapAccountMetadata
append_bootstrap_account(GenesisConfigurationBuilder &builder,
                         std::shared_ptr<const Contract> const &wallet_contract,
                         std::string_view seed, std::uint32_t index,
                         std::uint64_t storage_tokens,
                         BootstrapAccountRole role)
{
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
  auto keypair = derive_bootstrap_account_keypair(seed, index);
  SHA256Hash nonce = derive_wallet_nonce(seed, index);
  ContractAddress address =
      GenesisConfigurationLoader::calculate_deployed_address(
          sha256(*wallet_contract), nonce);

  builder.add_initial_config(wallet_contract, nonce,
                             wallet_meta::entrypoint<u8"initialize">(),
                             wallet_meta::make_calldata<u8"initialize">(
                                 keypair.first),
                             static_cast<int128_t>(storage_tokens));
  return BootstrapAccountMetadata{
      .index = index,
      .role = role,
      .address = address,
      .public_key = keypair.first,
      .initial_storage_tokens = storage_tokens,
      .storage_tokens = storage_tokens,
  };
}

BootstrapAccountArtifact
finish_bootstrap_artifact(GenesisConfigurationBuilder &builder,
                          std::vector<BootstrapAccountMetadata> accounts)
{
  std::optional<GenesisConfiguration> genesis_configuration =
      builder.build(StorageCostModel::default_cost_model());
  if (!genesis_configuration) {
    throw std::runtime_error("bootstrap accounts allocate more hogs than the "
                             "initial hog token supply");
  }

  return BootstrapAccountArtifact{
      .genesis_configuration = std::move(*genesis_configuration),
      .initialization_txs = builder.make_unfurl_genesis_transactions(),
      .accounts = std::move(accounts),
  };
}

} // namespace

std::pair<Ed25519PublicKey, Ed25519SecretKey>
derive_bootstrap_account_keypair(std::string_view seed, std::uint32_t index)
{
  SHA256Hash secret_seed = derive_account_secret_seed(seed, index);
  return ed25519_keygen_from_seed(std::as_bytes(std::span(secret_seed)));
}

BootstrapAccountArtifact
build_bootstrap_account_artifact(BootstrapAccountConfig const &config)
{
  GenesisConfigurationBuilder builder;
  std::vector<BootstrapAccountMetadata> accounts =
      append_bootstrap_accounts(builder, config);

  return finish_bootstrap_artifact(builder, std::move(accounts));
}

std::vector<BootstrapAccountMetadata>
append_bootstrap_accounts(GenesisConfigurationBuilder &builder,
                          BootstrapAccountConfig const &config,
                          BootstrapAccountRole role)
{
  CHECK_LE(config.count, kMaxBootstrapAccounts);

  DevWalletProfile wallet_profile =
      resolve_dev_wallet_profile(config.dev_wallet_profile);

  auto wallet_contract = load_dev_wallet_contract(wallet_profile);

  std::vector<BootstrapAccountMetadata> accounts;
  accounts.reserve(config.count);

  for (std::uint32_t i = 0; i < config.count; ++i) {
    accounts.push_back(append_bootstrap_account(builder, wallet_contract,
                                                config.seed, i,
                                                config.storage_tokens, role));
  }

  return accounts;
}

BootstrapAccountArtifact
build_local_bootstrap_account_artifact(LocalBootstrapConfig const &config)
{
  CHECK_LE(config.user_account_count, kMaxBootstrapAccounts);
  auto wallet_contract = load_dev_wallet_contract(
      resolve_dev_wallet_profile(config.dev_wallet_profile));
  GenesisConfigurationBuilder builder;
  std::vector<BootstrapAccountMetadata> accounts;
  accounts.reserve(static_cast<std::size_t>(config.user_account_count) + 1);
  accounts.push_back(append_bootstrap_account(builder, wallet_contract,
                                              config.seed, 0,
                                              kLocalFaucetStorageTokens,
                                              BootstrapAccountRole::kFaucet));
  for (std::uint32_t index = 1; index <= config.user_account_count; ++index) {
    accounts.push_back(
        append_bootstrap_account(builder, wallet_contract, config.seed, index,
                                 config.user_storage_tokens,
                                 BootstrapAccountRole::kLocalUser));
  }
  return finish_bootstrap_artifact(builder, std::move(accounts));
}

} // namespace hogd
