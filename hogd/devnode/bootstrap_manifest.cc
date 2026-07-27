#include "hogd/devnode/bootstrap_manifest.h"

#include "hogd/base/network_hash.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "built_contracts/dev_wallet.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/json.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/devnode/manifest_parse.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/version.h"
#include "types.hh"

namespace hogd {

namespace {

constexpr std::size_t kMaxBootstrapManifestBytes = 64 * 1024;
constexpr std::string_view kBootstrapManifestKind = "groundhog-bootstrap";
// v1 supports only the hog-devnode-default contract profile and the
// hogd-default cost-model profile. When benchmark profiles arrive (spec: Phase
// 5), this equality check generalizes into a lookup, and the per-profile rule
// that benchmark profiles must pin `protocol` becomes mandatory.
constexpr std::string_view kBootstrapProfile = "local-dev";
constexpr std::string_view kStorageCostModelProfile = "hogd-default";

std::expected<std::string, std::string>
read_bootstrap_manifest_file(std::filesystem::path const &path)
{
  // Open with O_NOFOLLOW to close the final-component TOCTOU window: if the
  // manifest path itself is a symlink, open() fails with ELOOP rather than
  // transparently following it. fstat() then operates on the same inode the fd
  // pins, so a racing rename/replace cannot redirect us at a different file.
  const int fd =
      ::open(path.c_str(),
             O_RDONLY | O_NOFOLLOW | O_CLOEXEC); // NOLINT(misc-include-cleaner)
  if (fd < 0) {
    const int err = errno;
    // ELOOP from O_NOFOLLOW means the final component was a symlink. We
    // surface this with the same "must be a regular file" message we used
    // for the symlink_status-rejection path so existing assertions still
    // match.
    if (err == ELOOP) {
      return std::unexpected(
          std::format("bootstrap manifest must be a regular file: {}",
                      path.string()));
    }
    return std::unexpected(
        std::format("failed to open bootstrap manifest: {}: {}", path.string(),
                    std::strerror(err)));
  }
  UniqueFd fd_guard(fd);

  struct stat st{};
  if (::fstat(fd, &st) < 0) {
    const int err = errno;
    return std::unexpected(
        std::format("failed to stat bootstrap manifest: {}: {}", path.string(),
                    std::strerror(err)));
  }
  if (!S_ISREG(st.st_mode)) {
    return std::unexpected(
        std::format("bootstrap manifest must be a regular file: {}",
                    path.string()));
  }

  // Read up to kMaxBootstrapManifestBytes + 1 bytes; the +1 lets us detect
  // oversize files without trusting st.st_size (which can be stale or, for
  // some fs types, zero for nonzero content).
  std::string bytes(kMaxBootstrapManifestBytes + 1, '\0');
  std::size_t total = 0;
  while (total < bytes.size()) {
    const auto n = ::read(fd, bytes.data() + total, bytes.size() - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int err = errno;
      return std::unexpected(
          std::format("failed to read bootstrap manifest: {}: {}",
                      path.string(), std::strerror(err)));
    }
    if (n == 0) {
      break;
    }
    total += static_cast<std::size_t>(n);
  }
  bytes.resize(total);
  if (bytes.size() > kMaxBootstrapManifestBytes) {
    return std::unexpected(std::format("bootstrap manifest exceeds {} bytes",
                                       kMaxBootstrapManifestBytes));
  }
  return bytes;
}

std::expected<const json::Value *, std::string>
require_object_field(const json::Value &parent, std::string_view key,
                     std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    return std::unexpected(
        std::format("bootstrap manifest missing {}.{}", context, key));
  }
  if (field->type() != json::Value::Type::kObject) {
    return std::unexpected(
        std::format("bootstrap manifest field {}.{} is not an object", context,
                    key));
  }
  return field;
}

std::expected<std::string_view, std::string>
require_string_field(const json::Value &parent, std::string_view key,
                     std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    return std::unexpected(
        std::format("bootstrap manifest missing {}.{}", context, key));
  }
  std::optional<std::string_view> value = field->as_string();
  if (!value.has_value()) {
    return std::unexpected(
        std::format("bootstrap manifest field {}.{} is not a string", context,
                    key));
  }
  return *value;
}

std::expected<std::optional<std::string_view>, std::string>
optional_string_field(const json::Value &parent, std::string_view key,
                      std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string_view> value = field->as_string();
  if (!value.has_value()) {
    return std::unexpected(
        std::format("bootstrap manifest field {}.{} is not a string", context,
                    key));
  }
  return value;
}

std::expected<std::int64_t, std::string>
require_integer_field(const json::Value &parent, std::string_view key,
                      std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    return std::unexpected(
        std::format("bootstrap manifest missing {}.{}", context, key));
  }
  std::optional<std::int64_t> value = field->as_integer();
  if (!value.has_value()) {
    return std::unexpected(
        std::format("bootstrap manifest field {}.{} is not an integer", context,
                    key));
  }
  return *value;
}

std::expected<void, std::string>
require_string_value(std::string_view actual, std::string_view expected,
                     std::string_view field)
{
  if (actual != expected) {
    return std::unexpected(
        std::format("unsupported bootstrap manifest {} '{}', expected '{}'",
                    field, actual, expected));
  }
  return {};
}

// hog-devnode only: this asserts that the manifest matches the dev-wallet
// artifact the binary already has. In a non-dev backend the relationship
// reverses: the manifest names the artifact bytes to deploy and the backend
// verifies its deployed sha256 against the manifest's contentHash.
std::expected<DevWalletProfile, std::string>
validate_dev_wallet_assertions(const json::Value &dev_wallet)
{
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

  std::expected<std::string_view, std::string> profile_name =
      require_string_field(dev_wallet, "profile", "devWallet");
  if (!profile_name) {
    return std::unexpected(std::move(profile_name.error()));
  }
  if (std::expected<void, std::string> valid =
          require_string_value(*profile_name, kDefaultDevWalletProfile,
                               "devWallet.profile");
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  DevWalletProfile profile = resolve_dev_wallet_profile(*profile_name);

  std::expected<std::optional<std::string_view>, std::string> content_hash =
      optional_string_field(dev_wallet, "contentHash", "devWallet");
  if (!content_hash) {
    return std::unexpected(std::move(content_hash.error()));
  }
  if (content_hash->has_value()) {
    auto wallet_contract = load_dev_wallet_contract(profile);
    const std::string wallet_hash = hex_string(sha256(*wallet_contract));
    if (std::expected<void, std::string> valid =
            require_string_value(**content_hash, wallet_hash,
                                 "devWallet.contentHash");
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
  }

  std::expected<std::optional<std::string_view>, std::string>
      deploy_entrypoint =
          optional_string_field(dev_wallet, "deployEntrypoint", "devWallet");
  if (!deploy_entrypoint) {
    return std::unexpected(std::move(deploy_entrypoint.error()));
  }
  if (deploy_entrypoint->has_value()) {
    if (std::expected<void, std::string> valid =
            require_string_value(**deploy_entrypoint,
                                 entrypoint_hex(
                                     wallet_meta::entrypoint<"deploy">()),
                                 "devWallet.deployEntrypoint");
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
  }

  std::expected<std::optional<std::string_view>, std::string>
      do_action_entrypoint =
          optional_string_field(dev_wallet, "doActionEntrypoint", "devWallet");
  if (!do_action_entrypoint) {
    return std::unexpected(std::move(do_action_entrypoint.error()));
  }
  if (do_action_entrypoint->has_value()) {
    if (std::expected<void, std::string> valid =
            require_string_value(**do_action_entrypoint,
                                 entrypoint_hex(
                                     wallet_meta::entrypoint<"do_action">()),
                                 "devWallet.doActionEntrypoint");
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
  }
  return profile;
}

} // namespace

std::expected<BootstrapManifestConfig, std::string>
load_bootstrap_manifest(std::filesystem::path const &path)
{
  std::expected<std::string, std::string> bytes =
      read_bootstrap_manifest_file(path);
  if (!bytes) {
    return std::unexpected(std::move(bytes.error()));
  }
  std::expected<json::Value, json::ParseError> parsed = json::parse(*bytes);
  if (!parsed.has_value()) {
    return std::unexpected(
        std::format("bootstrap manifest JSON parse failed at {}: {}",
                    parsed.error().offset, parsed.error().message));
  }
  if (parsed->type() != json::Value::Type::kObject) {
    return std::unexpected("bootstrap manifest root is not an object");
  }

  std::expected<std::int64_t, std::string> schema_version =
      require_integer_field(*parsed, "schemaVersion", "root");
  if (!schema_version) {
    return std::unexpected(std::move(schema_version.error()));
  }
  // Additive optional fields do not bump schemaVersion, so exact-match is
  // correct under the additive-only contract.
  if (*schema_version != 2) {
    return std::unexpected(std::format(
        "unsupported bootstrap manifest schemaVersion {}, expected 2",
        *schema_version));
  }

  std::expected<std::string_view, std::string> kind =
      require_string_field(*parsed, "kind", "root");
  if (!kind) {
    return std::unexpected(std::move(kind.error()));
  }
  if (std::expected<void, std::string> valid =
          require_string_value(*kind, kBootstrapManifestKind, "kind");
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }

  std::expected<std::string_view, std::string> network_hash_field =
      require_string_field(*parsed, "networkHash", "root");
  if (!network_hash_field) {
    return std::unexpected(std::move(network_hash_field.error()));
  }
  std::optional<NetworkHash> network_hash =
      parse_network_hash(*network_hash_field);
  if (!network_hash) {
    return std::unexpected("bootstrap manifest networkHash must be exactly 64 "
                           "hexadecimal characters");
  }

  std::expected<std::string_view, std::string> profile =
      require_string_field(*parsed, "profile", "root");
  if (!profile) {
    return std::unexpected(std::move(profile.error()));
  }
  if (std::expected<void, std::string> valid =
          require_string_value(*profile, kBootstrapProfile, "profile");
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }

  std::expected<std::optional<std::string_view>, std::string> protocol =
      optional_string_field(*parsed, "protocol", "root");
  if (!protocol) {
    return std::unexpected(std::move(protocol.error()));
  }
  if (protocol->has_value()) {
    if (std::expected<void, std::string> valid =
            require_string_value(**protocol, version::kHogdProtocol(),
                                 "protocol");
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
  }

  std::expected<const json::Value *, std::string> dev_wallet_field =
      require_object_field(*parsed, "devWallet", "root");
  if (!dev_wallet_field) {
    return std::unexpected(std::move(dev_wallet_field.error()));
  }
  std::expected<DevWalletProfile, std::string> dev_wallet_profile =
      validate_dev_wallet_assertions(**dev_wallet_field);
  if (!dev_wallet_profile) {
    return std::unexpected(std::move(dev_wallet_profile.error()));
  }

  std::expected<const json::Value *, std::string> storage_cost_model =
      require_object_field(*parsed, "storageCostModel", "root");
  if (!storage_cost_model) {
    return std::unexpected(std::move(storage_cost_model.error()));
  }
  std::expected<std::string_view, std::string> storage_cost_profile =
      require_string_field(**storage_cost_model, "profile", "storageCostModel");
  if (!storage_cost_profile) {
    return std::unexpected(std::move(storage_cost_profile.error()));
  }
  if (std::expected<void, std::string> valid =
          require_string_value(*storage_cost_profile, kStorageCostModelProfile,
                               "storageCostModel.profile");
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }

  std::expected<const json::Value *, std::string> accounts =
      require_object_field(*parsed, "bootstrapAccounts", "root");
  if (!accounts) {
    return std::unexpected(std::move(accounts.error()));
  }
  std::expected<std::int64_t, std::string> count =
      require_integer_field(**accounts, "count", "bootstrapAccounts");
  if (!count) {
    return std::unexpected(std::move(count.error()));
  }
  if (*count < 0 || std::cmp_greater(*count, kMaxBootstrapAccounts)) {
    return std::unexpected(
        std::format("bootstrap manifest bootstrapAccounts.count {} exceeds "
                    "maximum {}",
                    *count, kMaxBootstrapAccounts));
  }
  std::expected<std::string_view, std::string> seed_field =
      require_string_field(**accounts, "seed", "bootstrapAccounts");
  if (!seed_field) {
    return std::unexpected(std::move(seed_field.error()));
  }
  std::string seed(*seed_field);
  if (seed.empty()) {
    return std::unexpected(
        "bootstrap manifest bootstrapAccounts.seed must not be empty");
  }
  std::expected<std::string_view, std::string> initial_storage_tokens =
      require_string_field(**accounts, "initialStorageTokens",
                           "bootstrapAccounts");
  if (!initial_storage_tokens) {
    return std::unexpected(std::move(initial_storage_tokens.error()));
  }
  const std::optional<std::uint64_t> storage_tokens =
      try_parse_u64(*initial_storage_tokens);
  if (!storage_tokens) {
    return std::unexpected(
        std::format("Invalid bootstrap manifest initialStorageTokens: {}",
                    *initial_storage_tokens));
  }
  BootstrapAccountConfig config{
      .count = static_cast<std::uint32_t>(*count),
      .seed = std::move(seed),
      .storage_tokens = *storage_tokens,
      .dev_wallet_profile = std::string(dev_wallet_profile->name),
  };
  return BootstrapManifestConfig{
      .network_hash = *network_hash,
      .bootstrap_config = std::move(config),
  };
}

} // namespace hogd
