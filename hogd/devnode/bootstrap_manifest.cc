#include "hogd/devnode/bootstrap_manifest.h"

#include "hogd/base/base58.h"
#include "hogd/base/network_hash.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "hogd/base/cleanup.h"
#include "hogd/base/json.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/manifest_parse.h"
#include "hogd/version.h"
#include "hogd/xdr/protocol_token.h"
#include "types.hh"

namespace hogd {

namespace {

constexpr std::size_t kMaxBootstrapManifestBytes = 256 * 1024;
constexpr std::string_view kBootstrapManifestKind = "groundhog-bootstrap";
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

std::expected<void, std::string>
validate_fields(json::Value const &value,
                std::initializer_list<std::string_view> allowed,
                std::string_view context)
{
  if (value.type() != json::Value::Type::kObject) {
    return std::unexpected(std::format("{} must be an object", context));
  }
  std::set<std::string_view> seen;
  for (auto const &[name, unused] : value.as_object()) {
    if (std::ranges::find(allowed, name) == allowed.end()) {
      return std::unexpected(
          std::format("unknown {} field '{}'", context, name));
    }
    if (!seen.insert(name).second) {
      return std::unexpected(
          std::format("duplicate {} field '{}'", context, name));
    }
  }
  return {};
}

std::expected<LocalAccountAllocation, std::string>
parse_allocation(json::Value const &value, std::string_view context)
{
  if (auto valid = validate_fields(value, {"publicKey", "initialStorageTokens"},
                                   context);
      !valid) {
    return std::unexpected(valid.error());
  }
  auto key = require_string_field(value, "publicKey", context);
  if (!key) {
    return std::unexpected(key.error());
  }
  const auto public_key = decode_base58_fixed<32>(*key);
  if (!public_key) {
    return std::unexpected(
        std::format("{}.publicKey must be base58 encoding of exactly 32 bytes",
                    context));
  }
  LocalAccountAllocation allocation;
  allocation.public_key = *public_key;
  auto raw = require_string_field(value, "initialStorageTokens", context);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  auto amount = try_parse_u64(*raw);
  if (!amount || *amount == 0 || std::to_string(*amount) != *raw) {
    return std::unexpected(std::format(
        "{}.initialStorageTokens must be a positive canonical decimal amount",
        context));
  }
  allocation.storage_tokens = *amount;
  return allocation;
}

} // namespace

std::expected<LocalBootstrapConfig, std::string>
parse_local_bootstrap_accounts(json::Value const &value)
{
  auto faucet = require_object_field(value, "bootstrapFaucet", "root");
  if (!faucet) {
    return std::unexpected(faucet.error());
  }
  auto allocation = parse_allocation(**faucet, "bootstrapFaucet");
  if (!allocation) {
    return std::unexpected(allocation.error());
  }
  if (allocation->storage_tokens != kLocalFaucetStorageTokens) {
    return std::unexpected(
        std::format("bootstrapFaucet.initialStorageTokens must be {}",
                    kLocalFaucetStorageTokens));
  }
  auto const *accounts = value.find("bootstrapAccounts");
  if (accounts == nullptr || accounts->type() != json::Value::Type::kArray) {
    return std::unexpected("bootstrapAccounts must be an array");
  }
  if (accounts->as_array().size() > kMaxBootstrapAccounts) {
    return std::unexpected("bootstrapAccounts exceeds maximum account count");
  }
  LocalBootstrapConfig config{.faucet_public_key = allocation->public_key};
  std::set<Ed25519PublicKey> keys{config.faucet_public_key};
  for (auto const &account : accounts->as_array()) {
    auto parsed = parse_allocation(account, "bootstrapAccounts entry");
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    if (!keys.insert(parsed->public_key).second) {
      return std::unexpected(
          "duplicate bootstrap public key (including faucet)");
    }
    config.accounts.push_back(*parsed);
  }
  return config;
}

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
  if (*schema_version != 5) {
    return std::unexpected(std::format(
        "unsupported bootstrap manifest schemaVersion {}, expected 5; "
        "use the original runtime for legacy manifests or generate a new "
        "manifest",
        *schema_version));
  }
  if (auto valid = validate_fields(*parsed,
                                   {"schemaVersion", "kind", "networkHash",
                                    "profile", "protocol", "bootstrapFaucet",
                                    "bootstrapAccounts", "storageCostModel"},
                                   "root");
      !valid) {
    return std::unexpected(valid.error());
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
  if (!network_hash ||
      format_network_hash(*network_hash) != *network_hash_field) {
    return std::unexpected("bootstrap manifest networkHash must be base58 "
                           "encoding of exactly 32 bytes");
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
    if (std::expected<void, std::string> valid = require_string_value(
            **protocol, protocol_text(version::kHogdProtocol()), "protocol");
        !valid) {
      return std::unexpected(std::move(valid.error()));
    }
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

  if (auto valid = validate_fields(**storage_cost_model, {"profile"},
                                   "storageCostModel");
      !valid) {
    return std::unexpected(valid.error());
  }
  auto config = parse_local_bootstrap_accounts(*parsed);
  if (!config) {
    return std::unexpected(config.error());
  }
  return BootstrapManifestConfig{
      .network_hash = *network_hash,
      .bootstrap_config = std::move(*config),
  };
}

} // namespace hogd
