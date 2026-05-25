#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <print>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "built_contracts/dev_wallet.h"
#include "hogd/base/json.h"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogd_dev/bootstrap_accounts.h"
#include "hogd/hogd_dev/hogd_dev_node.h"
#include "hogd/hogd_dev/runtime_manifest.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/init.h"
#include "hogd/version.h"

namespace hogd {

namespace {

enum class ReadinessSink { kStdout };

// Darwin's POSIX signal declarations are not fully modeled by include-cleaner.
// NOLINTBEGIN(misc-include-cleaner)
volatile sig_atomic_t shutdown_requested =
    0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

struct ShutdownSignals {
  sigset_t suspend_mask;
};

void
request_shutdown(int signal_number)
{
  (void)signal_number;
  shutdown_requested = 1;
}

ShutdownSignals
block_shutdown_signal()
{
  sigset_t blocked_mask;
  CHECK_EQ(sigemptyset(&blocked_mask), 0);
  CHECK_EQ(sigaddset(&blocked_mask, SIGTERM), 0);

  sigset_t suspend_mask;
  CHECK_EQ(sigprocmask(SIG_BLOCK, &blocked_mask, &suspend_mask), 0);
  CHECK_EQ(sigdelset(&suspend_mask, SIGTERM), 0);

  return {.suspend_mask = suspend_mask};
}

void
install_signal_handler()
{
  struct sigaction action{};
  action.sa_handler = request_shutdown;
  CHECK_EQ(sigemptyset(&action.sa_mask), 0);
  CHECK_EQ(sigaction(SIGTERM, &action, nullptr), 0);
}

void
wait_for_shutdown(const ShutdownSignals &signals)
{
  while (!shutdown_requested) {
    // SIGTERM is blocked outside sigsuspend(), so checking the flag and
    // sleeping are atomic with respect to the shutdown signal.
    sigsuspend(&signals.suspend_mask);
  }
  info("hogd-dev shutdown requested");
  std::fflush(stderr);
}
// NOLINTEND(misc-include-cleaner)

void
emit_readiness(const HogdDevNode &node, ReadinessSink sink)
{
  RuntimeManifestPointer manifest = write_hogd_dev_runtime_manifest(node);
  switch (sink) {
  case ReadinessSink::kStdout:
    std::println("{}", manifest.readiness_payload);
    std::fflush(stdout);
  }
}

int
hogd_dev_init(std::string listen_host, std::uint16_t requested_scheduler_port,
              ReadinessSink readiness_sink, std::string runtime_network_id,
              BootstrapAccountConfig const &bootstrap_config)
{
  install_signal_handler();

  HogdDevNode node(std::move(listen_host), requested_scheduler_port,
                   std::move(runtime_network_id), std::move(bootstrap_config));
  emit_readiness(node, readiness_sink);
  info("hogd-dev initialization complete.");

  ShutdownSignals shutdown_signals = block_shutdown_signal();
  wait_for_shutdown(shutdown_signals);
  return EXIT_SUCCESS;
}

bool
is_wildcard_listen_host(std::string_view host)
{
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }

  std::string host_string(host);
  in_addr ipv4{};
  if (inet_pton(AF_INET, host_string.c_str(), &ipv4) == 1) {
    return ipv4.s_addr == INADDR_ANY;
  }

  in6_addr ipv6{};
  if (inet_pton(AF_INET6, host_string.c_str(), &ipv6) == 1) {
    return IN6_IS_ADDR_UNSPECIFIED(&ipv6);
  }

  return false;
}

} // namespace

} // namespace hogd

namespace {

struct HogdDevOptions {
  std::string listen_host = "127.0.0.1";
  std::string runtime_network_id = "groundhog-local";
  std::uint16_t scheduler_port = 0;
  hogd::ReadinessSink readiness_sink = hogd::ReadinessSink::kStdout;
  hogd::BootstrapAccountConfig bootstrap_config;
};

struct BootstrapManifestConfig {
  std::string network_id;
  hogd::BootstrapAccountConfig bootstrap_config;
};

struct OptionSources {
  bool bootstrap_manifest = false;
  bool dev_accounts = false;
  bool dev_account_seed = false;
  bool dev_account_tokens = false;
};

constexpr std::size_t kMaxBootstrapManifestBytes = 64 * 1024;
constexpr std::string_view kBootstrapManifestKind = "groundhog-bootstrap";
constexpr std::string_view kBootstrapProfile = "local-dev";
constexpr std::string_view kDevWalletProfile = "hogd-dev-default";
constexpr std::string_view kStorageCostModelProfile = "hogd-default";

void
print_usage(FILE *stream, char *argv[])
{
  std::println(stream, R"(Usage: {} [OPTIONS]

Start an ephemeral local Groundhog development node.

Options:
  --listen-host <ip>             Host/interface for internal x23 services
                                 [default: 127.0.0.1]
  --scheduler-port <port>        SchedulerCoord port; use 0 for an OS port
                                 [default: 0]
  --readiness stdout             Emit one readiness JSON line on stdout
                                 [default: stdout]
  --bootstrap-manifest <path>    Bootstrap intent manifest. When set,
                                 dev-account shorthand flags are conflicts.
  --dev-accounts <count>         Deterministic dev accounts to bootstrap
                                 [default: 0]
  --dev-account-seed <seed>      Seed phrase for deterministic dev accounts
                                 [default: groundhog-dev-accounts]
  --dev-account-tokens <tokens>  Initial storage-token balance per dev account
                                 [default: 1000000000]
  -h, --help                     Print help

Environment:
  HOGD_LOG_FORMAT=json           Emit newline-delimited JSON logs to stderr.
                                 The default is human-readable stderr logs.

Readiness:
  stdout is reserved for the readiness line when --readiness stdout is used.
  Operational logs are written to stderr.)",
               argv[0]);
}

[[noreturn]] void
usage(char *argv[])
{
  print_usage(stderr, argv);
  exit(EXIT_FAILURE);
}

std::uint16_t
parse_port(const std::string &value)
{
  std::size_t pos = 0;
  std::uint64_t port = 0;
  try {
    port = std::stoul(value, &pos, 10);
  } catch (const std::exception &) {
    throw std::runtime_error(std::format("Invalid port: {}", value));
  }
  if (pos != value.size() || port > 65535) {
    throw std::runtime_error(std::format("Invalid port: {}", value));
  }
  return static_cast<std::uint16_t>(port);
}

std::uint32_t
parse_u32(const std::string &value, std::string_view name)
{
  std::size_t pos = 0;
  std::uint64_t parsed = 0;
  try {
    parsed = std::stoull(value, &pos, 10);
  } catch (const std::exception &) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  if (pos != value.size() ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  return static_cast<std::uint32_t>(parsed);
}

std::uint32_t
parse_bootstrap_account_count(const std::string &value)
{
  std::uint32_t count = parse_u32(value, "dev account count");
  if (count > hogd::kMaxBootstrapAccounts) {
    throw std::runtime_error(
        std::format("Invalid dev account count: {} exceeds maximum {}", value,
                    hogd::kMaxBootstrapAccounts));
  }
  return count;
}

std::uint64_t
parse_u64(const std::string &value, std::string_view name)
{
  std::size_t pos = 0;
  std::uint64_t parsed = 0;
  try {
    parsed = std::stoull(value, &pos, 10);
  } catch (const std::exception &) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  if (pos != value.size() ||
      parsed > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  return parsed;
}

std::string
hex_string(std::span<const std::byte> bytes)
{
  return std::format("0x{}", hogd::encode_hex(bytes));
}

template<std::size_t N>
std::string
hex_string(std::array<std::uint8_t, N> const &bytes)
{
  return hex_string(std::as_bytes(std::span(bytes)));
}

std::string
entrypoint_hex(ContractEntrypointID const &entrypoint)
{
  return hex_string(std::as_bytes(std::span(entrypoint)));
}

std::string
read_bootstrap_manifest_file(std::filesystem::path const &path)
{
  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, status_error);
  if (status_error) {
    throw std::runtime_error(
        std::format("failed to stat bootstrap manifest {}: {}", path.string(),
                    status_error.message()));
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw std::runtime_error(
        std::format("bootstrap manifest must be a regular file: {}",
                    path.string()));
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(
        std::format("failed to open bootstrap manifest: {}", path.string()));
  }

  std::string bytes(kMaxBootstrapManifestBytes + 1, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(input.gcount()));
  if (bytes.size() > kMaxBootstrapManifestBytes) {
    throw std::runtime_error(std::format("bootstrap manifest exceeds {} bytes",
                                         kMaxBootstrapManifestBytes));
  }
  if (input.bad()) {
    throw std::runtime_error(
        std::format("failed to read bootstrap manifest: {}", path.string()));
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

void
validate_dev_wallet_assertions(const json::Value &dev_wallet)
{
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

  require_string_value(require_string_field(dev_wallet, "profile", "devWallet"),
                       kDevWalletProfile, "devWallet.profile");

  auto wallet_contract = wallet_meta::load_wasm();
  CHECK(wallet_contract != nullptr);
  const std::string wallet_hash = hex_string(sha256(*wallet_contract));

  if (std::optional<std::string_view> content_hash =
          optional_string_field(dev_wallet, "contentHash", "devWallet")) {
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

BootstrapManifestConfig
bootstrap_config_from_manifest(std::filesystem::path const &path)
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
  if (count < 0 ||
      std::cmp_greater(count, std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("Invalid bootstrap manifest account count");
  }
  hogd::BootstrapAccountConfig config{
      .count = parse_bootstrap_account_count(std::to_string(count)),
      .seed = std::string(
          require_string_field(accounts, "seed", "bootstrapAccounts")),
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

void
reject_manifest_shorthand_conflicts(OptionSources const &sources)
{
  if (!sources.bootstrap_manifest) {
    return;
  }
  if (sources.dev_accounts || sources.dev_account_seed ||
      sources.dev_account_tokens) {
    throw std::runtime_error(
        "--bootstrap-manifest conflicts with dev-account shorthand flags");
  }
}

HogdDevOptions
parse_args(int argc, char *argv[])
{
  HogdDevOptions options;
  OptionSources sources;
  std::optional<std::filesystem::path> bootstrap_manifest_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(stdout, argv);
      exit(EXIT_SUCCESS);
    }
    if (arg == "--listen-host" && i + 1 < argc) {
      options.listen_host = argv[++i];
      if (hogd::is_wildcard_listen_host(options.listen_host)) {
        throw std::runtime_error(
            "Invalid listen host: wildcard addresses cannot be advertised");
      }
      continue;
    }
    if (arg == "--scheduler-port" && i + 1 < argc) {
      options.scheduler_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--readiness" && i + 1 < argc) {
      const std::string readiness = argv[++i];
      if (readiness != "stdout") {
        throw std::runtime_error(
            std::format("Invalid readiness sink: {}", readiness));
      }
      options.readiness_sink = hogd::ReadinessSink::kStdout;
      continue;
    }
    if (arg == "--bootstrap-manifest" && i + 1 < argc) {
      if (sources.bootstrap_manifest) {
        throw std::runtime_error(
            "--bootstrap-manifest specified more than once");
      }
      sources.bootstrap_manifest = true;
      bootstrap_manifest_path = std::filesystem::path(argv[++i]);
      continue;
    }
    if (arg == "--dev-accounts" && i + 1 < argc) {
      sources.dev_accounts = true;
      options.bootstrap_config.count = parse_bootstrap_account_count(argv[++i]);
      continue;
    }
    if (arg == "--dev-account-seed" && i + 1 < argc) {
      sources.dev_account_seed = true;
      options.bootstrap_config.seed = argv[++i];
      continue;
    }
    if (arg == "--dev-account-tokens" && i + 1 < argc) {
      sources.dev_account_tokens = true;
      options.bootstrap_config.storage_tokens =
          parse_u64(argv[++i], "dev account tokens");
      continue;
    }
    usage(argv);
  }
  reject_manifest_shorthand_conflicts(sources);
  if (bootstrap_manifest_path.has_value()) {
    BootstrapManifestConfig manifest_config =
        bootstrap_config_from_manifest(*bootstrap_manifest_path);
    options.runtime_network_id = std::move(manifest_config.network_id);
    options.bootstrap_config = std::move(manifest_config.bootstrap_config);
  }
  return options;
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  HogdDevOptions options;
  try {
    options = parse_args(argc, argv);
  } catch (const std::exception &error) {
    std::println(stderr, "error: {}", error.what());
    print_usage(stderr, argv);
    return EXIT_FAILURE;
  }

  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogd-dev!");

  try {
    return hogd::hogd_dev_init(std::move(options.listen_host),
                               options.scheduler_port, options.readiness_sink,
                               std::move(options.runtime_network_id),
                               options.bootstrap_config);
  } catch (const std::exception &error) {
    hogd::error("hogd-dev failed before readiness: {}", error.what());
    return EXIT_FAILURE;
  }
}
