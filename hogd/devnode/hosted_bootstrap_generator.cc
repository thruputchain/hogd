#include "hogd/devnode/hosted_bootstrap_generator.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "built_contracts/dev_wallet.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/io.h"
#include "hogd/base/json.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

namespace {

constexpr std::string_view kStorageCostModelHashTag =
    "groundhog-storage-cost-model-v1";
constexpr int kHostedBootstrapTempOpenFlags =
    O_WRONLY | O_CREAT | O_TRUNC; // NOLINT(misc-include-cleaner)

std::string
xdr_hex(std::span<const std::byte> bytes)
{
  return encode_hex(bytes);
}

template<std::size_t N>
std::string
xdr_hex(std::array<std::uint8_t, N> const &bytes)
{
  return xdr_hex(std::as_bytes(std::span(bytes)));
}

std::string
uint64_string(std::uint64_t value)
{
  return std::to_string(value);
}

std::string
int128_to_decimal(int128_t value)
{
  if (value == 0) {
    return "0";
  }

  const bool negative = value < 0;
  uint128_t remaining = negative ? static_cast<uint128_t>(-(value + 1)) + 1
                                 : static_cast<uint128_t>(value);

  std::string out;
  while (remaining != 0) {
    const auto digit = static_cast<unsigned>(remaining % 10);
    out.push_back(static_cast<char>('0' + digit));
    remaining /= 10;
  }
  if (negative) {
    out.push_back('-');
  }
  std::ranges::reverse(out);
  return out;
}

void
hash_field(SHA256Hasher &hasher, std::string_view value)
{
  hasher.update(std::as_bytes(std::span(value.data(), value.size())));
  constexpr char kSeparator = '\0';
  hasher.update(std::as_bytes(std::span(&kSeparator, 1)));
}

SHA256Hash
storage_cost_model_hash(StorageCostModel const &model)
{
  SHA256Hasher hasher;
  hash_field(hasher, kStorageCostModelHashTag);
  hash_field(hasher, int128_to_decimal(model.kBaseItemCost));
  hash_field(hasher, std::to_string(model.kBytestringCostPerByte));
  hash_field(hasher, int128_to_decimal(model.kBaseContractDeploymentCost));
  hash_field(hasher, int128_to_decimal(model.kContractDeploymentCostPerByte));
  hash_field(hasher, int128_to_decimal(model.kAuthPerKeyCost));
  hash_field(hasher, int128_to_decimal(model.kRCQCostPerEntry));
  hash_field(hasher, int128_to_decimal(model.kReplayCacheCostPerEntry));
  hash_field(hasher, int128_to_decimal(model.kBalanceCost));
  return hasher.finalize();
}

void
write_storage_cost_model(json::Writer &w, StorageCostModel const &model)
{
  w.field("hash", hex_string(storage_cost_model_hash(model)));
  w.field("baseDataItemCost", int128_to_decimal(model.kBaseItemCost));
  w.field("bytestringCostPerByte",
          std::to_string(model.kBytestringCostPerByte));
  w.field("baseContractDeploymentCost",
          int128_to_decimal(model.kBaseContractDeploymentCost));
  w.field("contractDeploymentCostPerByte",
          int128_to_decimal(model.kContractDeploymentCostPerByte));
  w.field("authPerKeyCost", int128_to_decimal(model.kAuthPerKeyCost));
  w.field("rcqCostPerEntry", int128_to_decimal(model.kRCQCostPerEntry));
  w.field("replayCacheCostPerEntry",
          int128_to_decimal(model.kReplayCacheCostPerEntry));
  w.field("balanceCost", int128_to_decimal(model.kBalanceCost));
}

void
write_storage_cost_model_xdr(json::Writer &w, StorageCostModelXdr const &model)
{
  w.field("base_data_item_cost", uint64_string(model.base_data_item_cost));
  w.field("bytestring_cost_per_byte",
          static_cast<std::int64_t>(model.bytestring_cost_per_byte));
  w.field("base_contract_deployment_cost",
          uint64_string(model.base_contract_deployment_cost));
  w.field("contract_deployment_cost_per_byte",
          uint64_string(model.contract_deployment_cost_per_byte));
  w.field("auth_per_key_cost", uint64_string(model.auth_per_key_cost));
  w.field("rcq_cost_per_entry", uint64_string(model.rcq_cost_per_entry));
  w.field("replay_cache_cost_per_entry",
          uint64_string(model.replay_cache_cost_per_entry));
  w.field("balance_cost", uint64_string(model.balance_cost));
}

void
write_int128_xdr(json::Writer &w, int128_xdr const &value)
{
  w.field("highbits", static_cast<std::int64_t>(value.highbits));
  w.field("lowbits", uint64_string(value.lowbits));
}

std::string
build_genesis_json(GenesisConfiguration const &genesis)
{
  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.array_field("initial_config", [&](json::Writer &w) {
      for (InitialConfigurationEntry const &entry : genesis.initial_config) {
        w.object([&](json::Writer &w) {
          w.object_field("config", [&](json::Writer &w) {
            w.field("initial_nonce", xdr_hex(entry.config.initial_nonce));
            w.field("contract_hash", xdr_hex(entry.config.contract_hash));
            w.field("entrypoint", xdr_hex(entry.config.entrypoint));
            w.field("calldata",
                    xdr_hex(std::as_bytes(std::span(entry.config.calldata))));
          });
          w.object_field("amount", [&](json::Writer &w) {
            write_int128_xdr(w, entry.amount);
          });
        });
      }
    });
    w.object_field("storage_cost_model", [&](json::Writer &w) {
      write_storage_cost_model_xdr(w, genesis.storage_cost_model);
    });
  });
  return buffer;
}

std::string
build_metadata_json(HostedBootstrapGeneratorConfig const &config,
                    HostedBootstrapArtifact const &artifact)
{
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

  DevWalletProfile wallet_profile =
      resolve_dev_wallet_profile(config.bootstrap_config.dev_wallet_profile);
  auto wallet_contract = load_dev_wallet_contract(wallet_profile);
  const SHA256Hash dev_wallet_hash = sha256(*wallet_contract);
  const StorageCostModel storage_cost_model(
      artifact.genesis_configuration.storage_cost_model);

  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("schemaVersion", std::int64_t{1});
    w.field("kind", "hog-hosted-bootstrap");
    w.field("networkId", config.network_id);
    w.field("protocol", version::kHogdProtocol());
    w.field("genesisHash", hex_string(artifact.genesis_hash));
    w.object_field("bootstrapConfig", [&](json::Writer &w) {
      w.field("devWalletProfile", config.bootstrap_config.dev_wallet_profile);
      w.field("devAccounts",
              static_cast<std::int64_t>(config.bootstrap_config.count));
      w.field("devAccountSeed", config.bootstrap_config.seed);
      w.field("devAccountTokens",
              std::to_string(config.bootstrap_config.storage_tokens));
    });
    w.object_field("bootstrapAccountDerivation", [&](json::Writer &w) {
      w.field("scheme", "groundhog-bootstrap-account-v1");
      w.field("seed", config.bootstrap_config.seed);
    });
    w.array_field("bootstrapAccounts", [&](json::Writer &w) {
      for (BootstrapAccountMetadata const &account : artifact.accounts) {
        w.object([&](json::Writer &w) {
          w.field("index", static_cast<std::int64_t>(account.index));
          w.field("role", "hosted_dev_account");
          w.field("publicKey", hex_string(account.public_key));
          w.field("walletAddress", hex_string(account.address));
          w.field("initialStorageTokens",
                  std::to_string(account.initial_storage_tokens));
        });
      }
    });
    w.array_field("initializationTransactions", [&](json::Writer &w) {
      for (std::size_t i = 0; i < artifact.initialization_txs.size(); ++i) {
        w.object([&](json::Writer &w) {
          x23::Buf tx_xdr = x23::serialize(artifact.initialization_txs[i]);
          w.field("index", static_cast<std::int64_t>(i));
          w.field("hash",
                  hex_string(sha256(artifact.initialization_txs[i].tx)));
          w.field("xdr", xdr_hex(std::as_bytes(
                             std::span(tx_xdr.data(), tx_xdr.size()))));
        });
      }
    });
    w.object_field("bootstrap", [&](json::Writer &w) {
      w.object_field("devWallet", [&](json::Writer &w) {
        w.field("contentHash", hex_string(dev_wallet_hash));
        w.field("deployedCodeHash", hex_string(dev_wallet_hash));
        w.field("artifactHint", wallet_profile.artifact_hint);
        w.field("deployEntrypoint",
                entrypoint_hex(wallet_meta::entrypoint<"deploy">()));
        w.field("doActionEntrypoint",
                entrypoint_hex(wallet_meta::entrypoint<"do_action">()));
        w.field("createWalletEntrypoint",
                entrypoint_hex(wallet_meta::entrypoint<"create_wallet">()));
      });
      w.object_field("storageCostModel", [&](json::Writer &w) {
        write_storage_cost_model(w, storage_cost_model);
      });
    });
  });
  return buffer;
}

std::optional<std::string>
read_file_if_exists(std::filesystem::path const &path)
{
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return std::nullopt;
  }
  if (ec) {
    throw std::runtime_error(
        std::format("could not inspect '{}': {}", path.string(), ec.message()));
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(std::format("failed to open {}", path.string()));
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

[[noreturn]] void
throw_io_error(std::string_view operation, Errno const &error)
{
  throw std::runtime_error(std::format("{} failed: {}", operation, error));
}

template<typename T>
T
require_io(ErrnoStatus<T> status, std::string_view operation)
{
  if (!status.has_value()) {
    throw_io_error(operation, status.error());
  }
  return std::move(*status);
}

void
require_io(ErrnoStatus<void> status, std::string_view operation)
{
  if (!status.has_value()) {
    throw_io_error(operation, status.error());
  }
}

void
write_file_atomically(std::filesystem::path const &path,
                      std::string_view contents)
{
  std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }

  Directory dir = require_io(Directory::open(parent.string(), /*create=*/false),
                             "open hosted bootstrap output directory");
  const std::string filename = path.filename().string();
  const std::string tmp_filename = std::format("{}.tmp", filename);
  Defer cleanup_tmp([&] { std::ignore = dir.unlink(tmp_filename); });
  File file =
      require_io(dir.openat(tmp_filename, kHostedBootstrapTempOpenFlags, 0644),
                 "open hosted bootstrap temp file");

  std::span<const std::byte> remaining =
      std::as_bytes(std::span(contents.data(), contents.size()));
  while (!remaining.empty()) {
    const std::size_t count =
        require_io(file.write(remaining), "write hosted bootstrap temp file");
    if (count == 0) {
      throw std::runtime_error(
          "write hosted bootstrap temp file wrote 0 bytes");
    }
    remaining = remaining.subspan(count);
  }
  require_io(file.sync(), "fsync hosted bootstrap temp file");
  require_io(file.close(), "close hosted bootstrap temp file");
  require_io(dir.rename(tmp_filename, filename),
             "rename hosted bootstrap file");
  require_io(dir.sync(), "fsync hosted bootstrap output directory");
}

void
write_file_if_absent_or_same(std::filesystem::path const &path,
                             std::string_view contents)
{
  std::optional<std::string> existing = read_file_if_exists(path);
  if (existing.has_value()) {
    if (*existing != contents) {
      throw std::runtime_error(std::format(
          "refusing to overwrite mismatched hosted bootstrap file: {}",
          path.string()));
    }
    return;
  }

  std::error_code ec;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      throw std::runtime_error(
          std::format("could not create directory '{}': {}",
                      path.parent_path().string(), ec.message()));
    }
  }

  write_file_atomically(path, contents);
}

} // namespace

HostedBootstrapArtifact
build_hosted_bootstrap_artifact(HostedBootstrapGeneratorConfig const &config)
{
  if (config.network_id.empty()) {
    throw std::runtime_error("hosted bootstrap network id must not be empty");
  }
  if (config.bootstrap_config.seed.empty()) {
    throw std::runtime_error(
        "hosted bootstrap dev account seed must not be empty");
  }
  resolve_dev_wallet_profile(config.bootstrap_config.dev_wallet_profile);

  BootstrapAccountArtifact account_artifact;
  if (config.bootstrap_config.count == 0) {
    GenesisConfigurationBuilder builder;
    account_artifact.genesis_configuration =
        builder.build(StorageCostModel::default_cost_model());
  }
  else {
    account_artifact =
        build_bootstrap_account_artifact(config.bootstrap_config);
  }

  HostedBootstrapArtifact artifact{
      .genesis_configuration =
          std::move(account_artifact.genesis_configuration),
      .initialization_txs = std::move(account_artifact.initialization_txs),
      .accounts = std::move(account_artifact.accounts),
  };
  artifact.genesis_hash = sha256(artifact.genesis_configuration);
  artifact.genesis_json = build_genesis_json(artifact.genesis_configuration);
  artifact.metadata_json = build_metadata_json(config, artifact);
  return artifact;
}

void
write_hosted_bootstrap_artifact(HostedBootstrapGeneratorConfig const &config,
                                std::filesystem::path const &genesis_out,
                                std::filesystem::path const &metadata_out)
{
  if (genesis_out.empty()) {
    throw std::runtime_error("--genesis-out path must not be empty");
  }
  if (metadata_out.empty()) {
    throw std::runtime_error("--metadata-out path must not be empty");
  }

  HostedBootstrapArtifact artifact = build_hosted_bootstrap_artifact(config);
  write_file_if_absent_or_same(genesis_out, artifact.genesis_json);
  write_file_if_absent_or_same(metadata_out, artifact.metadata_json);
}

} // namespace hogd
