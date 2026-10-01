#include "hogd/devnode/bootstrap_accounts.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "types.hh"

namespace hogd {

namespace {

ConfigParamID
local_parameter_id(std::string_view name)
{
  if (const auto id = config_param_id(name)) {
    return *id;
  }
  throw std::runtime_error(
      std::format("unknown network configuration parameter '{}'", name));
}

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

BootstrapAccountMetadata
append_bootstrap_account(GenesisConfigurationBuilder &builder,
                         std::string_view seed, std::uint32_t index,
                         std::uint64_t storage_tokens,
                         BootstrapAccountRole role)
{
  auto keypair = derive_bootstrap_account_keypair(seed, index);
  ContractAddress address = derive_bootstrap_account_address(keypair.first);
  builder.add_account_allocation(address,
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
                          std::vector<BootstrapAccountMetadata> accounts,
                          NetworkHash const &network_hash,
                          StorageCostModel const &cost_model,
                          NetworkConfig const &network_config)
{
  auto initialization_txs =
      builder.try_make_genesis_ceremony_transactions(cost_model);
  if (!initialization_txs.has_value()) {
    throw std::runtime_error(
        "bootstrap accounts and ceremony funding exceed the native token "
        "supply");
  }
  return BootstrapAccountArtifact{
      .genesis_configuration =
          builder.build(network_hash, cost_model,
                        *load_stock_genesis_root_contract(), network_config),
      .initialization_txs = std::move(*initialization_txs),
      .accounts = std::move(accounts),
  };
}

} // namespace

void
add_local_network_config_override(LocalNetworkConfigOverrides &overrides,
                                  std::string_view assignment)
{
  const auto separator = assignment.find('=');
  if (separator == std::string_view::npos) {
    throw std::runtime_error("--network-config expects NAME=VALUE");
  }
  const auto name = assignment.substr(0, separator);
  const auto raw = assignment.substr(separator + 1);
  static_cast<void>(local_parameter_id(name));
  std::uint32_t value = 0;
  const auto [end, error] =
      std::from_chars(raw.data(), raw.data() + raw.size(), value);
  if (error != std::errc{} || end != raw.data() + raw.size()) {
    throw std::runtime_error(
        std::format("network parameter '{}' requires a decimal uint32 value",
                    name));
  }
  if (!overrides.emplace(std::string(name), value).second) {
    throw std::runtime_error(
        std::format("network parameter '{}' specified more than once", name));
  }
}

NetworkConfig
local_network_config(LocalNetworkConfigOverrides const &overrides)
{
  auto config = stock_network_config();
  for (auto const &[name, value] : overrides) {
    NetworkConfigParams::set_param(config, local_parameter_id(name), value);
  }
  return config;
}

std::pair<Ed25519PublicKey, Ed25519SecretKey>
derive_bootstrap_account_keypair(std::string_view seed, std::uint32_t index)
{
  SHA256Hash secret_seed = derive_account_secret_seed(seed, index);
  return ed25519_keygen_from_seed(std::as_bytes(std::span(secret_seed)));
}

ContractAddress
derive_bootstrap_account_address(Ed25519PublicKey const &public_key)
{
  PublicKey key(CryptographicScheme::Ed25519);
  key.ed25519() = public_key;
  auto address =
      pubkey_auth_address(PubkeyAccountPreimage{.pubkey = key, .nonce = 0});
  if (!address.has_value()) {
    throw std::logic_error("native bootstrap address derivation failed");
  }
  return *address;
}

BootstrapAccountArtifact
build_bootstrap_account_artifact(BootstrapAccountConfig const &config,
                                 NetworkHash const &network_hash,
                                 StorageCostModel const &cost_model)
{
  GenesisConfigurationBuilder builder;
  std::vector<BootstrapAccountMetadata> accounts =
      append_bootstrap_accounts(builder, config);

  return finish_bootstrap_artifact(builder, std::move(accounts), network_hash,
                                   cost_model, stock_network_config());
}

std::vector<BootstrapAccountMetadata>
append_bootstrap_accounts(GenesisConfigurationBuilder &builder,
                          BootstrapAccountConfig const &config,
                          BootstrapAccountRole role)
{
  CHECK_LE(config.count, kMaxBootstrapAccounts);

  std::vector<BootstrapAccountMetadata> accounts;
  accounts.reserve(config.count);

  for (std::uint32_t i = 0; i < config.count; ++i) {
    accounts.push_back(append_bootstrap_account(builder, config.seed, i,
                                                config.storage_tokens, role));
  }

  return accounts;
}

BootstrapAccountArtifact
build_local_bootstrap_account_artifact(LocalBootstrapConfig const &config,
                                       NetworkHash const &network_hash,
                                       StorageCostModel const &cost_model)
{
  CHECK_LE(config.user_account_count, kMaxBootstrapAccounts);
  GenesisConfigurationBuilder builder;
  std::vector<BootstrapAccountMetadata> accounts;
  accounts.reserve(static_cast<std::size_t>(config.user_account_count) + 1);
  accounts.push_back(append_bootstrap_account(builder, config.seed, 0,
                                              kLocalFaucetStorageTokens,
                                              BootstrapAccountRole::kFaucet));
  for (std::uint32_t index = 1; index <= config.user_account_count; ++index) {
    accounts.push_back(
        append_bootstrap_account(builder, config.seed, index,
                                 config.user_storage_tokens,
                                 BootstrapAccountRole::kLocalUser));
  }
  return finish_bootstrap_artifact(builder, std::move(accounts), network_hash,
                                   cost_model,
                                   local_network_config(
                                       config.network_config_overrides));
}

} // namespace hogd
