#include "hogd/devnode/runtime_manifest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "built_contracts/dev_wallet.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/io.h"
#include "hogd/base/json.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/hog_devnode_node.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "types.hh"

namespace hogd {

namespace {

static_assert('A' == 0x41 && '0' == 0x30 && '_' == 0x5F,
              "ordinary literal encoding must be ASCII-compatible");

constexpr std::string_view kManifestFileName = "runtime-manifest.json";
constexpr std::string_view kManifestTmpSuffix = ".tmp";
constexpr std::string_view kStorageCostModelHashTag =
    "groundhog-storage-cost-model-v1";
constexpr int kManifestTempOpenFlags =
    O_WRONLY | O_CREAT | O_TRUNC; // NOLINT(misc-include-cleaner)

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

SHA256Hash
hash_bytes(std::string_view bytes)
{
  SHA256Hasher hasher;
  hasher.update(std::as_bytes(std::span(bytes.data(), bytes.size())));
  return hasher.finalize();
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

std::string
build_runtime_manifest_json(HogDevnodeNode const &node,
                            std::string_view network_id)
{
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

  DevWalletProfile wallet_profile =
      resolve_dev_wallet_profile(node.bootstrap_config().dev_wallet_profile);
  auto wallet_contract = load_dev_wallet_contract(wallet_profile);
  const SHA256Hash dev_wallet_hash = sha256(*wallet_contract);
  const StorageCostModel storage_cost_model(
      node.genesis_configuration().storage_cost_model);
  const SHA256Hash genesis_hash = sha256(node.genesis_configuration());

  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("schemaVersion", std::int64_t{1});
    w.field("kind", "hog-devnode-runtime");
    w.field("networkId", network_id);
    w.field("protocol", version::kHogdProtocol());
    w.field("genesisHash", hex_string(genesis_hash));
    w.object_field("producer", [&](json::Writer &w) {
      w.field("binary", "hog-devnode");
      w.field("version", version::kHogdVersion);
      std::string_view commit_hash = version::kHogdGitHash;
      if (!commit_hash.empty() && commit_hash != "unknown") {
        w.field("commitHash", commit_hash);
      }
    });
    w.object_field("endpoints", [&](json::Writer &w) {
      w.field("scheduler", format_advertised_address(node.bind_host(),
                                                     node.scheduler_port()));
      w.field("indexer",
              format_advertised_address(node.bind_host(), node.read_port()));
      w.field("dbGateway",
              format_advertised_address(node.bind_host(), node.gateway_port()));
      w.field("viewGateway",
              format_advertised_address(node.bind_host(),
                                        node.view_gateway_port()));
    });
    w.object_field("bootstrapAccountDerivation", [&](json::Writer &w) {
      w.field("scheme", "groundhog-bootstrap-account-v1");
      w.field("seed", node.bootstrap_config().seed);
    });
    w.array_field("bootstrapAccounts", [&](json::Writer &w) {
      for (BootstrapAccountMetadata const &account :
           node.bootstrap_accounts()) {
        w.object([&](json::Writer &w) {
          w.field("index", static_cast<std::int64_t>(account.index));
          w.field("role", "local_user");
          w.field("publicKey", hex_string(account.public_key));
          w.field("walletAddress", hex_string(account.address));
          w.field("initialStorageTokens",
                  std::to_string(account.initial_storage_tokens));
        });
      }
    });
    w.object_field("bootstrap", [&](json::Writer &w) {
      w.object_field("devWallet", [&](json::Writer &w) {
        w.field("contentHash", hex_string(dev_wallet_hash));
        w.field("deployedCodeHash", hex_string(dev_wallet_hash));
        w.field("artifactHint", wallet_profile.artifact_hint);
        w.field("deployEntrypoint",
                entrypoint_hex(wallet_meta::entrypoint<"initialize">()));
        w.field("doActionEntrypoint",
                entrypoint_hex(wallet_meta::entrypoint<"do_action">()));
      });
      w.object_field("storageCostModel", [&](json::Writer &w) {
        write_storage_cost_model(w, storage_cost_model);
      });
    });
  });
  return buffer;
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
ignore_io_error(ErrnoStatus<void> status)
{
  (void)status.has_value();
}

void
write_manifest_atomically(std::filesystem::path const &path,
                          std::string_view bytes)
{
  Directory dir =
      require_io(Directory::open(path.parent_path().string(), /*create=*/false),
                 "open runtime manifest directory");
  const std::string filename = path.filename().string();
  const std::string tmp_filename =
      std::format("{}{}", filename, kManifestTmpSuffix);

  bool renamed = false;
  Defer cleanup_tmp([&] {
    if (!renamed) {
      ignore_io_error(dir.unlink(tmp_filename));
    }
  });

  File file = require_io(dir.openat(tmp_filename, kManifestTempOpenFlags, 0600),
                         "open runtime manifest temp file");
  std::span<const std::byte> remaining =
      std::as_bytes(std::span(bytes.data(), bytes.size()));
  while (!remaining.empty()) {
    std::size_t count =
        require_io(file.write(remaining), "write runtime manifest temp file");
    if (count == 0) {
      throw std::runtime_error(
          "write runtime manifest temp file wrote 0 bytes");
    }
    remaining = remaining.subspan(count);
  }
  require_io(file.sync(), "fsync runtime manifest temp file");
  require_io(file.close(), "close runtime manifest temp file");
  require_io(dir.rename(tmp_filename, filename), "rename runtime manifest");
  renamed = true;
  require_io(dir.sync(), "fsync runtime manifest directory");
}

std::string
build_readiness_payload(std::filesystem::path const &manifest_path,
                        SHA256Hash const &manifest_hash)
{
  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("schemaVersion", std::int64_t{1});
    w.field("status", "ready");
    w.field("manifestPath", manifest_path.string());
    w.field("manifestHash", hex_string(manifest_hash));
  });
  return buffer;
}

} // namespace

std::string
format_hex_hash(SHA256Hash const &hash)
{
  return hex_string(hash);
}

RuntimeManifestPointer
write_hog_devnode_runtime_manifest(HogDevnodeNode const &node,
                                   std::string_view network_id)
{
  std::filesystem::path manifest_path =
      std::filesystem::path(node.runtime_dir()) / kManifestFileName;
  std::string manifest = build_runtime_manifest_json(node, network_id);
  write_manifest_atomically(manifest_path, manifest);

  SHA256Hash manifest_hash = hash_bytes(manifest);
  std::string readiness_payload =
      build_readiness_payload(manifest_path, manifest_hash);
  return RuntimeManifestPointer{
      .manifest_path = std::move(manifest_path),
      .manifest_hash = manifest_hash,
      .readiness_payload = std::move(readiness_payload),
  };
}

} // namespace hogd
