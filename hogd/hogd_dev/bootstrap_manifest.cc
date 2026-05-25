#include "hogd/hogd_dev/bootstrap_manifest.h"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "built_contracts/dev_wallet.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/json.h"
#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogd_dev/bootstrap_accounts.h"
#include "hogd/hogd_dev/manifest_hex.h"
#include "hogd/hogd_dev/manifest_parse.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/version.h"

namespace hogd {

namespace {

constexpr std::size_t kMaxBootstrapManifestBytes = 64 * 1024;
constexpr std::string_view kBootstrapManifestKind = "groundhog-bootstrap";
// v1 supports only the hogd-dev-default contract profile and the hogd-default
// cost-model profile. When benchmark profiles arrive (spec: Phase 5), this
// equality check generalizes into a lookup, and the per-profile rule that
// benchmark profiles must pin `protocol` becomes mandatory.
constexpr std::string_view kBootstrapProfile = "local-dev";
constexpr std::string_view kDevWalletProfile = "hogd-dev-default";
constexpr std::string_view kStorageCostModelProfile = "hogd-default";

std::string
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
      throw std::runtime_error(
          std::format("bootstrap manifest must be a regular file: {}",
                      path.string()));
    }
    throw std::runtime_error(
        std::format("failed to open bootstrap manifest: {}: {}", path.string(),
                    std::strerror(err)));
  }
  UniqueFd fd_guard(fd);

  struct stat st{};
  if (::fstat(fd, &st) < 0) {
    const int err = errno;
    throw std::runtime_error(
        std::format("failed to stat bootstrap manifest: {}: {}", path.string(),
                    std::strerror(err)));
  }
  if (!S_ISREG(st.st_mode)) {
    throw std::runtime_error(
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
      throw std::runtime_error(
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
    throw std::runtime_error(std::format("bootstrap manifest exceeds {} bytes",
                                         kMaxBootstrapManifestBytes));
  }
  return bytes;
}

const json::Value &
require_object_field(const json::Value &parent, std::string_view key,
                     std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("bootstrap manifest missing {}.{}", context, key));
  }
  if (field->type() != json::Value::Type::kObject) {
    throw std::runtime_error(
        std::format("bootstrap manifest field {}.{} is not an object", context,
                    key));
  }
  return *field;
}

std::string_view
require_string_field(const json::Value &parent, std::string_view key,
                     std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("bootstrap manifest missing {}.{}", context, key));
  }
  std::optional<std::string_view> value = field->as_string();
  if (!value.has_value()) {
    throw std::runtime_error(
        std::format("bootstrap manifest field {}.{} is not a string", context,
                    key));
  }
  return *value;
}

std::optional<std::string_view>
optional_string_field(const json::Value &parent, std::string_view key,
                      std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string_view> value = field->as_string();
  if (!value.has_value()) {
    throw std::runtime_error(
        std::format("bootstrap manifest field {}.{} is not a string", context,
                    key));
  }
  return value;
}

std::int64_t
require_integer_field(const json::Value &parent, std::string_view key,
                      std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("bootstrap manifest missing {}.{}", context, key));
  }
  std::optional<std::int64_t> value = field->as_integer();
  if (!value.has_value()) {
    throw std::runtime_error(
        std::format("bootstrap manifest field {}.{} is not an integer", context,
                    key));
  }
  return *value;
}

void
require_string_value(std::string_view actual, std::string_view expected,
                     std::string_view field)
{
  if (actual != expected) {
    throw std::runtime_error(
        std::format("unsupported bootstrap manifest {} '{}', expected '{}'",
                    field, actual, expected));
  }
}

// hogd-dev only: this asserts that the manifest matches the dev-wallet WASM
// blob the binary already has. In a non-dev backend the relationship
// reverses — the manifest names the artifact bytes to deploy and the
// backend verifies its deployed sha256 against the manifest's contentHash.
void
validate_dev_wallet_assertions(const json::Value &dev_wallet)
{
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

  require_string_value(require_string_field(dev_wallet, "profile", "devWallet"),
                       kDevWalletProfile, "devWallet.profile");

  if (std::optional<std::string_view> content_hash =
          optional_string_field(dev_wallet, "contentHash", "devWallet")) {
    auto wallet_contract = wallet_meta::load_wasm();
    CHECK(wallet_contract != nullptr);
    const std::string wallet_hash = hex_string(sha256(*wallet_contract));
    require_string_value(*content_hash, wallet_hash, "devWallet.contentHash");
  }
  if (std::optional<std::string_view> deploy_entrypoint =
          optional_string_field(dev_wallet, "deployEntrypoint", "devWallet")) {
    require_string_value(*deploy_entrypoint,
                         entrypoint_hex(
                             wallet_meta::entrypoint<"initialize">()),
                         "devWallet.deployEntrypoint");
  }
  if (std::optional<std::string_view> do_action_entrypoint =
          optional_string_field(dev_wallet, "doActionEntrypoint",
                                "devWallet")) {
    require_string_value(*do_action_entrypoint,
                         entrypoint_hex(wallet_meta::entrypoint<"do_action">()),
                         "devWallet.doActionEntrypoint");
  }
}

} // namespace

BootstrapManifestConfig
load_bootstrap_manifest(std::filesystem::path const &path)
{
  std::string bytes = read_bootstrap_manifest_file(path);
  std::expected<json::Value, json::ParseError> parsed = json::parse(bytes);
  if (!parsed.has_value()) {
    throw std::runtime_error(
        std::format("bootstrap manifest JSON parse failed at {}: {}",
                    parsed.error().offset, parsed.error().message));
  }
  if (parsed->type() != json::Value::Type::kObject) {
    throw std::runtime_error("bootstrap manifest root is not an object");
  }

  const std::int64_t schema_version =
      require_integer_field(*parsed, "schemaVersion", "root");
  // Additive optional fields do not bump schemaVersion, so exact-match is
  // correct under the additive-only contract.
  if (schema_version != 1) {
    throw std::runtime_error(std::format(
        "unsupported bootstrap manifest schemaVersion {}, expected 1",
        schema_version));
  }
  require_string_value(require_string_field(*parsed, "kind", "root"),
                       kBootstrapManifestKind, "kind");
  std::string network_id =
      std::string(require_string_field(*parsed, "networkId", "root"));
  if (network_id.empty()) {
    throw std::runtime_error("bootstrap manifest networkId must not be empty");
  }
  require_string_value(require_string_field(*parsed, "profile", "root"),
                       kBootstrapProfile, "profile");
  if (std::optional<std::string_view> protocol =
          optional_string_field(*parsed, "protocol", "root")) {
    require_string_value(*protocol, version::kHogdProtocol(), "protocol");
  }

  validate_dev_wallet_assertions(
      require_object_field(*parsed, "devWallet", "root"));
  const json::Value &storage_cost_model =
      require_object_field(*parsed, "storageCostModel", "root");
  require_string_value(require_string_field(storage_cost_model, "profile",
                                            "storageCostModel"),
                       kStorageCostModelProfile, "storageCostModel.profile");

  const json::Value &accounts =
      require_object_field(*parsed, "bootstrapAccounts", "root");
  const std::int64_t count =
      require_integer_field(accounts, "count", "bootstrapAccounts");
  if (count < 0 || std::cmp_greater(count, kMaxBootstrapAccounts)) {
    throw std::runtime_error(
        std::format("bootstrap manifest bootstrapAccounts.count {} exceeds "
                    "maximum {}",
                    count, kMaxBootstrapAccounts));
  }
  std::string seed =
      std::string(require_string_field(accounts, "seed", "bootstrapAccounts"));
  if (seed.empty()) {
    throw std::runtime_error(
        "bootstrap manifest bootstrapAccounts.seed must not be empty");
  }
  BootstrapAccountConfig config{
      .count = static_cast<std::uint32_t>(count),
      .seed = std::move(seed),
      .storage_tokens =
          parse_u64(std::string(require_string_field(
                        accounts, "initialStorageTokens", "bootstrapAccounts")),
                    "bootstrap manifest initialStorageTokens"),
  };
  return {
      .network_id = std::move(network_id),
      .bootstrap_config = std::move(config),
  };
}

} // namespace hogd
