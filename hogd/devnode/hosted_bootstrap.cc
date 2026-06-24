#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/hosted_bootstrap_generator.h"
#include "hogd/devnode/manifest_parse.h"
#include "hogd/init.h"

namespace {

struct GenerateOptions {
  hogd::HostedBootstrapGeneratorConfig config;
  std::filesystem::path genesis_out;
  std::filesystem::path metadata_out;
  std::vector<std::filesystem::path> contract_artifact_directories;
};

void
print_usage(FILE *stream, char *argv[])
{
  std::println(stream, R"(Usage: {} generate [OPTIONS]

Generate hosted devnet bootstrap genesis and metadata without starting services.

Options:
  --network-id <id>              Network id for metadata [default: devnet]
  --dev-wallet-profile <name>    Built-in dev wallet profile
                                 [default: hog-devnode-default]
  --dev-accounts <count>         Deterministic dev accounts to bootstrap
                                 [default: 10]
  --dev-account-seed <seed>      Seed phrase for deterministic dev accounts
                                 [default: groundhog-devnet]
  --dev-account-tokens <tokens>  Initial storage-token balance per dev account
                                 [default: 1000000000]
  --genesis-out <path>           Output JSON-XDR genesis configuration
  --metadata-out <path>          Output hosted bootstrap metadata
  --contract-artifacts-dir <dir> Search this directory before default contract
                                 artifact locations. May be repeated.
  --version                      Print version
  -h, --help                     Print help)",
               argv[0]);
}

[[noreturn]] void
usage(char *argv[])
{
  print_usage(stderr, argv);
  std::exit(EXIT_FAILURE);
}

std::uint32_t
parse_u32(const std::string &value, std::string_view name)
{
  const std::uint64_t parsed = hogd::parse_u64(value, name);
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
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

GenerateOptions
parse_generate_args(int argc, char *argv[])
{
  GenerateOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(stdout, argv);
      std::exit(EXIT_SUCCESS);
    }
    if (arg == "--network-id" && i + 1 < argc) {
      options.config.network_id = argv[++i];
      if (options.config.network_id.empty()) {
        throw std::runtime_error("Invalid network id: id must not be empty");
      }
      continue;
    }
    if (arg == "--dev-wallet-profile" && i + 1 < argc) {
      options.config.bootstrap_config.dev_wallet_profile = argv[++i];
      if (options.config.bootstrap_config.dev_wallet_profile.empty()) {
        throw std::runtime_error(
            "Invalid dev wallet profile: profile must not be empty");
      }
      hogd::resolve_dev_wallet_profile(
          options.config.bootstrap_config.dev_wallet_profile);
      continue;
    }
    if (arg == "--dev-accounts" && i + 1 < argc) {
      options.config.bootstrap_config.count =
          parse_bootstrap_account_count(argv[++i]);
      continue;
    }
    if (arg == "--dev-account-seed" && i + 1 < argc) {
      options.config.bootstrap_config.seed = argv[++i];
      if (options.config.bootstrap_config.seed.empty()) {
        throw std::runtime_error(
            "Invalid dev account seed: seed must not be empty");
      }
      continue;
    }
    if (arg == "--dev-account-tokens" && i + 1 < argc) {
      options.config.bootstrap_config.storage_tokens =
          hogd::parse_u64(argv[++i], "dev account tokens");
      continue;
    }
    if (arg == "--genesis-out" && i + 1 < argc) {
      options.genesis_out = std::filesystem::path(argv[++i]);
      continue;
    }
    if (arg == "--metadata-out" && i + 1 < argc) {
      options.metadata_out = std::filesystem::path(argv[++i]);
      continue;
    }
    if (arg == "--contract-artifacts-dir" && i + 1 < argc) {
      std::filesystem::path dir(argv[++i]);
      if (dir.empty()) {
        throw std::runtime_error(
            "Invalid contract artifacts dir: path must not be empty");
      }
      options.contract_artifact_directories.push_back(std::move(dir));
      continue;
    }
    usage(argv);
  }

  if (options.genesis_out.empty()) {
    throw std::runtime_error("--genesis-out is required");
  }
  if (options.metadata_out.empty()) {
    throw std::runtime_error("--metadata-out is required");
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
  if (argc < 2 || std::string_view(argv[1]) != "generate") {
    print_usage(stderr, argv);
    return EXIT_FAILURE;
  }

  GenerateOptions options;
  try {
    options = parse_generate_args(argc, argv);
  } catch (const std::exception &error) {
    std::println(stderr, "error: {}", error.what());
    print_usage(stderr, argv);
    return EXIT_FAILURE;
  }

  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }

  try {
    hogd::set_contract_artifact_directories(
        options.contract_artifact_directories);
    hogd::write_hosted_bootstrap_artifact(options.config, options.genesis_out,
                                          options.metadata_out);
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    hogd::error("hog-hosted-bootstrap generate failed: {}", error.what());
    return EXIT_FAILURE;
  }
}
