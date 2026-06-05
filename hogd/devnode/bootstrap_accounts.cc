#include "hogd/devnode/bootstrap_accounts.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/dev_wallet.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/runtime/contract_trait.h"
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
  CHECK_LE(config.count, kMaxBootstrapAccounts);

  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

  auto wallet_contract = wallet_meta::load_wasm();
  CHECK(wallet_contract != nullptr);
  const SHA256Hash wallet_contract_hash = sha256(*wallet_contract);

  GenesisConfigurationBuilder builder;
  std::vector<BootstrapAccountMetadata> accounts;
  accounts.reserve(config.count);

  for (std::uint32_t i = 0; i < config.count; ++i) {
    auto keypair = derive_bootstrap_account_keypair(config.seed, i);
    const Ed25519PublicKey &public_key = keypair.first;
    SHA256Hash nonce = derive_wallet_nonce(config.seed, i);
    ContractAddress address =
        GenesisConfigurationLoader::calculate_deployed_address(
            wallet_contract_hash, nonce);

    builder.add_initial_config(wallet_contract, nonce,
                               wallet_meta::entrypoint<"initialize">(),
                               wallet_meta::make_calldata<"initialize">(
                                   public_key),
                               static_cast<int128_t>(config.storage_tokens));
    accounts.push_back(BootstrapAccountMetadata{
        .index = i,
        .address = address,
        .public_key = public_key,
        .initial_storage_tokens = config.storage_tokens,
        .storage_tokens = config.storage_tokens,
    });
  }

  return BootstrapAccountArtifact{
      .genesis_configuration =
          builder.build(StorageCostModel::default_cost_model()),
      .initialization_txs = builder.make_unfurl_genesis_transactions(),
      .accounts = std::move(accounts),
  };
}

} // namespace hogd
