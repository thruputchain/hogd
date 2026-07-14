#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "hogd/base/cli.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/bootstrap_manifest.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/hog_devnode_node.h"
#include "hogd/devnode/manifest_parse.h"
#include "hogd/devnode/runtime_manifest.h"
#include "hogd/hogexec/shared.h"
#include "hogd/init.h"

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
  // Use pthread_sigmask (not sigprocmask, whose behavior is unspecified in a
  // multithreaded process). This must run while the process is still
  // single-threaded so every worker thread spawned afterwards inherits the
  // blocked mask; see hog_devnode_init.
  CHECK_EQ(pthread_sigmask(SIG_BLOCK, &blocked_mask, &suspend_mask), 0);
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
  info("hog-devnode shutdown requested");
  std::fflush(stderr);
}
// NOLINTEND(misc-include-cleaner)

void
emit_readiness(const HogDevnodeNode &node, ReadinessSink sink,
               std::string_view runtime_network_id)
{
  RuntimeManifestPointer manifest =
      write_hog_devnode_runtime_manifest(node, runtime_network_id);
  switch (sink) {
  case ReadinessSink::kStdout:
    std::println("{}", manifest.readiness_payload);
    std::fflush(stdout);
  }
}

int
hog_devnode_init(std::string listen_host,
                 std::uint16_t requested_scheduler_port,
                 ReadinessSink readiness_sink,
                 std::string_view runtime_network_id,
                 BootstrapAccountConfig const &bootstrap_config,
                 std::optional<std::filesystem::path> const &data_dir)
{
  install_signal_handler();
  // Block SIGTERM before spawning any worker threads (init_hogexec_metrics and
  // the HogDevnodeNode constructor start well over a hundred of them). Threads
  // inherit the signal mask of their creator, so blocking here means SIGTERM is
  // blocked in every worker. A process-directed SIGTERM is then delivered only
  // to the main thread inside sigsuspend(). Otherwise the kernel could deliver
  // it to a worker thread; request_shutdown() would set the flag there, but the
  // main thread's sigsuspend() only wakes when the signal is delivered to
  // itself, so shutdown would hang until the test escalates to SIGKILL.
  ShutdownSignals shutdown_signals = block_shutdown_signal();

  init_hogexec_metrics();

  HogDevnodeNode node(std::move(listen_host), requested_scheduler_port,
                      bootstrap_config, data_dir);
  emit_readiness(node, readiness_sink, runtime_network_id);
  info("hog-devnode initialization complete.");

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

struct HogDevnodeOptions {
  std::string listen_host = "127.0.0.1";
  std::string runtime_network_id = std::string(hogd::kDefaultRuntimeNetworkId);
  std::uint16_t scheduler_port = 0;
  hogd::ReadinessSink readiness_sink = hogd::ReadinessSink::kStdout;
  std::optional<std::filesystem::path> data_dir;
  hogd::BootstrapAccountConfig bootstrap_config;
  std::vector<std::filesystem::path> contract_artifact_directories;
  std::optional<hogd::LogLevel> log_level;
};

class DataDirLock {
public:
  explicit DataDirLock(std::optional<std::filesystem::path> const &data_dir)
  {
    if (!data_dir.has_value()) {
      return;
    }

    std::error_code ec;
    std::filesystem::create_directories(*data_dir, ec);
    if (ec) {
      throw std::runtime_error(
          std::format("could not create hog-devnode data directory '{}': {}",
                      data_dir->string(), ec.message()));
    }
    if (!std::filesystem::is_directory(*data_dir, ec)) {
      if (ec) {
        throw std::runtime_error(
            std::format("could not inspect hog-devnode data directory '{}': {}",
                        data_dir->string(), ec.message()));
      }
      throw std::runtime_error(
          std::format("hog-devnode data path '{}' is not a directory",
                      data_dir->string()));
    }

    const std::filesystem::path lock_path = *data_dir / ".hog-devnode.lock";
    const int fd =
        ::open(lock_path.c_str(),
               O_RDWR | O_CREAT | O_CLOEXEC, // NOLINT(misc-include-cleaner)
               0600);
    if (fd < 0) {
      throw std::runtime_error(
          std::format("failed to open hog-devnode data-dir lock '{}': {}",
                      lock_path.string(), std::strerror(errno)));
    }
    lock_file_.emplace(fd);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int err = errno;
      if (err == EWOULDBLOCK || err == EAGAIN) {
        throw std::runtime_error(std::format(
            "hog-devnode data dir '{}' is already locked by another process",
            data_dir->string()));
      }
      throw std::runtime_error(
          std::format("failed to lock hog-devnode data dir '{}': {}",
                      data_dir->string(), std::strerror(err)));
    }
  }

private:
  std::optional<hogd::File> lock_file_;
};

struct OptionSources {
  bool bootstrap_manifest = false;
  bool dev_accounts = false;
  bool dev_account_seed = false;
  bool dev_account_tokens = false;
  bool dev_wallet_profile = false;
};

void
print_usage(FILE *stream, char *argv[])
{
  std::println(stream, R"(Usage: {} [OPTIONS]

Start a local Groundhog development node.

Options:
  --listen-host <ip>             Host/interface for internal x23 services
                                 [default: 127.0.0.1]
  --scheduler-port <port>        SchedulerCoord port; use 0 for an OS port
                                 [default: 0]
  --data-dir <dir>               Persist node state under this directory.
                                 Reusing a populated directory recovers the
                                 committed chain and requires the same
                                 bootstrap flags. Omit for an ephemeral node.
  --readiness stdout             Emit one readiness JSON line on stdout
                                 [default: stdout]
  --contract-artifacts-dir <dir> Search this directory before default contract
                                 artifact locations. May be repeated.
  --dev-wallet-profile <name>    Built-in dev wallet profile
                                 [default: hog-devnode-default]
  --bootstrap-manifest <path>    Bootstrap intent manifest. When set,
                                 dev-account and wallet shorthand flags
                                 conflict.
  --dev-accounts <count>         Deterministic dev accounts to bootstrap
                                 [default: 0]
  --dev-account-seed <seed>      Seed phrase for deterministic dev accounts
                                 [default: groundhog-dev-accounts]
  --dev-account-tokens <tokens>  Initial storage-token balance per dev account
                                 [default: 1000000000]
  --log-level <level>            Log verbosity: critical, error, warn, info,
                                 debug, or trace [default: build-dependent]
  --version                      Print version
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
parse_port(std::string_view value, char *argv[])
{
  std::optional<std::uint16_t> port = hogd::try_parse_port(value);
  if (!port) {
    hogd::arg_error(std::format("Invalid port: {}", value), argv, usage);
  }
  return *port;
}

std::uint32_t
parse_u32(std::string_view value, std::string_view name, char *argv[])
{
  std::optional<std::uint32_t> parsed =
      hogd::try_parse_integer<std::uint32_t>(value);
  if (!parsed) {
    hogd::arg_error(std::format("Invalid {}: {}", name, value), argv, usage);
  }
  return *parsed;
}

std::string_view
log_level_name(hogd::LogLevel level)
{
  using enum hogd::LogLevel;
  switch (level) {
  case kCritical:
    return "critical";
  case kError:
    return "error";
  case kWarn:
    return "warn";
  case kInfo:
    return "info";
  case kDebug:
    return "debug";
  case kTrace:
    return "trace";
  }
  return "unknown";
}

hogd::LogLevel
parse_log_level(std::string_view value, char *argv[])
{
  using enum hogd::LogLevel;
  hogd::LogLevel level;
  if (value == "critical")
    level = kCritical;
  else if (value == "error")
    level = kError;
  else if (value == "warn")
    level = kWarn;
  else if (value == "info")
    level = kInfo;
  else if (value == "debug")
    level = kDebug;
  else if (value == "trace")
    level = kTrace;
  else
    hogd::arg_error(std::format("Invalid log level: {}", value), argv, usage);

  // Levels above the compile-time ceiling are stripped from this build, so a
  // request for them can never be honored. Reject it rather than silently
  // capping.
  if (!hogd::should_log(level)) {
    hogd::arg_error(std::format("Log level '{}' is unavailable in this build; "
                                "the maximum verbosity "
                                "compiled in is '{}'",
                                value, log_level_name(hogd::kMaxLogLevel)),
                    argv, usage);
  }
  return level;
}

std::uint32_t
parse_bootstrap_account_count(std::string_view value, char *argv[])
{
  std::uint32_t count = parse_u32(value, "dev account count", argv);
  if (count > hogd::kMaxBootstrapAccounts) {
    hogd::arg_error(
        std::format("Invalid dev account count: {} exceeds maximum {}", value,
                    hogd::kMaxBootstrapAccounts),
        argv, usage);
  }
  return count;
}

void
reject_manifest_shorthand_conflicts(OptionSources const &sources, char *argv[])
{
  if (!sources.bootstrap_manifest) {
    return;
  }
  if (sources.dev_accounts || sources.dev_account_seed ||
      sources.dev_account_tokens || sources.dev_wallet_profile) {
    hogd::arg_error("--bootstrap-manifest conflicts with dev-account and "
                    "wallet shorthand flags",
                    argv, usage);
  }
}

HogDevnodeOptions
parse_args(int argc, char *argv[])
{
  HogDevnodeOptions options;
  OptionSources sources;
  std::optional<std::filesystem::path> bootstrap_manifest_path;
  std::optional<std::string> dev_accounts_value;
  std::optional<std::string> dev_account_seed_value;
  std::optional<std::string> dev_account_tokens_value;
  std::optional<std::string> dev_wallet_profile_value;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(stdout, argv);
      exit(EXIT_SUCCESS);
    }
    if (arg == "--listen-host" && i + 1 < argc) {
      options.listen_host = argv[++i];
      if (hogd::is_wildcard_listen_host(options.listen_host)) {
        hogd::arg_error(
            "Invalid listen host: wildcard addresses cannot be advertised",
            argv, usage);
      }
      continue;
    }
    if (arg == "--scheduler-port" && i + 1 < argc) {
      options.scheduler_port = parse_port(argv[++i], argv);
      continue;
    }
    if (arg == "--data-dir" && i + 1 < argc) {
      if (options.data_dir.has_value()) {
        hogd::arg_error("--data-dir specified more than once", argv, usage);
      }
      std::filesystem::path dir(argv[++i]);
      if (dir.empty()) {
        hogd::arg_error("Invalid data dir: path must not be empty", argv,
                        usage);
      }
      options.data_dir = std::move(dir);
      continue;
    }
    if (arg == "--readiness" && i + 1 < argc) {
      const std::string readiness = argv[++i];
      if (readiness != "stdout") {
        hogd::arg_error(std::format("Invalid readiness sink: {}", readiness),
                        argv, usage);
      }
      options.readiness_sink = hogd::ReadinessSink::kStdout;
      continue;
    }
    if (arg == "--contract-artifacts-dir" && i + 1 < argc) {
      std::filesystem::path dir(argv[++i]);
      if (dir.empty()) {
        hogd::arg_error(
            "Invalid contract artifacts dir: path must not be empty", argv,
            usage);
      }
      options.contract_artifact_directories.push_back(std::move(dir));
      continue;
    }
    if (arg == "--dev-wallet-profile" && i + 1 < argc) {
      if (sources.dev_wallet_profile) {
        hogd::arg_error("--dev-wallet-profile specified more than once", argv,
                        usage);
      }
      sources.dev_wallet_profile = true;
      dev_wallet_profile_value = argv[++i];
      continue;
    }
    if (arg == "--bootstrap-manifest" && i + 1 < argc) {
      if (sources.bootstrap_manifest) {
        hogd::arg_error("--bootstrap-manifest specified more than once", argv,
                        usage);
      }
      sources.bootstrap_manifest = true;
      bootstrap_manifest_path = std::filesystem::path(argv[++i]);
      continue;
    }
    if (arg == "--log-level" && i + 1 < argc) {
      options.log_level = parse_log_level(argv[++i], argv);
      continue;
    }
    if (arg == "--dev-accounts" && i + 1 < argc) {
      sources.dev_accounts = true;
      dev_accounts_value = argv[++i];
      continue;
    }
    if (arg == "--dev-account-seed" && i + 1 < argc) {
      sources.dev_account_seed = true;
      dev_account_seed_value = argv[++i];
      continue;
    }
    if (arg == "--dev-account-tokens" && i + 1 < argc) {
      sources.dev_account_tokens = true;
      dev_account_tokens_value = argv[++i];
      continue;
    }
    usage(argv);
  }
  reject_manifest_shorthand_conflicts(sources, argv);
  if (dev_accounts_value.has_value()) {
    options.bootstrap_config.count =
        parse_bootstrap_account_count(*dev_accounts_value, argv);
  }
  if (dev_account_seed_value.has_value()) {
    if (dev_account_seed_value->empty()) {
      hogd::arg_error("Invalid dev account seed: seed must not be empty", argv,
                      usage);
    }
    options.bootstrap_config.seed = std::move(*dev_account_seed_value);
  }
  if (dev_account_tokens_value.has_value()) {
    std::optional<std::uint64_t> tokens =
        hogd::try_parse_u64(*dev_account_tokens_value);
    if (!tokens) {
      hogd::arg_error(std::format("Invalid dev account tokens: {}",
                                  *dev_account_tokens_value),
                      argv, usage);
    }
    options.bootstrap_config.storage_tokens = *tokens;
  }
  if (dev_wallet_profile_value.has_value()) {
    if (dev_wallet_profile_value->empty()) {
      hogd::arg_error("Invalid dev wallet profile: profile must not be empty",
                      argv, usage);
    }
    hogd::resolve_dev_wallet_profile(*dev_wallet_profile_value);
    options.bootstrap_config.dev_wallet_profile =
        std::move(*dev_wallet_profile_value);
  }
  hogd::set_contract_artifact_directories(
      options.contract_artifact_directories);
  if (bootstrap_manifest_path.has_value()) {
    std::expected<hogd::BootstrapManifestConfig, std::string> manifest_config =
        hogd::load_bootstrap_manifest(*bootstrap_manifest_path);
    if (!manifest_config) {
      hogd::arg_error(manifest_config.error(), argv, usage);
    }
    options.runtime_network_id = std::move(manifest_config->network_id);
    options.bootstrap_config = std::move(manifest_config->bootstrap_config);
  }
  return options;
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (hogd::print_version_and_exit_if_requested(argc, argv)) {
    return EXIT_SUCCESS;
  }
  HogDevnodeOptions options;
  try {
    options = parse_args(argc, argv);
  } catch (const std::exception &error) {
    std::println(stderr, "error: {}", error.what());
    print_usage(stderr, argv);
    return EXIT_FAILURE;
  }

  int ret = options.log_level.has_value()
                ? hogd::hogd_init(argc, argv, true, *options.log_level)
                : hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hog-devnode!");

  try {
    DataDirLock data_dir_lock(options.data_dir);
    return hogd::hog_devnode_init(std::move(options.listen_host),
                                  options.scheduler_port,
                                  options.readiness_sink,
                                  options.runtime_network_id,
                                  options.bootstrap_config, options.data_dir);
  } catch (const std::exception &error) {
    hogd::error("hog-devnode failed before readiness: {}", error.what());
    return EXIT_FAILURE;
  }
}
