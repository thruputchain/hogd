#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "hogd/base/cli.h"
#include "hogd/base/log.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/hosted_bootstrap_generator.h"
#include "hogd/devnode/manifest_parse.h"
#include "hogd/init.h"
#include "types.hh"

namespace {

struct GenerateOptions {
  hogd::HostedBootstrapGeneratorConfig config;
  std::filesystem::path genesis_out;
  std::filesystem::path metadata_out;
  std::vector<std::filesystem::path> contract_artifact_directories;
};

// Defined below, after the usage text; the parse helpers report through it.
[[noreturn]] void usage(char *argv[]);

hogd::Ed25519PublicKey
parse_faucet_public_key(std::string_view value, char *argv[])
{
  if (value.starts_with("0x")) {
    value.remove_prefix(2);
  }
  if (value.size() != 64) {
    hogd::arg_error("Invalid faucet public key: expected 32-byte hex value",
                    argv, usage);
  }

  hogd::Ed25519PublicKey public_key{};
  for (std::size_t i = 0; i < public_key.size(); ++i) {
    unsigned int byte = 0;
    const char *first = value.data() + (i * 2);
    const char *last = first + 2;
    auto [ptr, error] = std::from_chars(first, last, byte, 16);
    if (error != std::errc{} || ptr != last) {
      hogd::arg_error("Invalid faucet public key: expected 32-byte hex value",
                      argv, usage);
    }
    public_key[i] = static_cast<std::uint8_t>(byte);
  }
  return public_key;
}

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
                                 [default: groundhog-dev-accounts]
  --dev-account-tokens <tokens>  Initial storage-token balance per dev account
                                 [default: 1000000000]
  --faucet-public-key <hex>      Dedicated faucet Ed25519 public key
  --faucet-tokens <tokens>       Initial storage-token balance for the faucet
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
parse_u32(std::string_view value, std::string_view name, char *argv[])
{
  std::optional<std::uint32_t> parsed =
      hogd::try_parse_integer<std::uint32_t>(value);
  if (!parsed) {
    hogd::arg_error(std::format("Invalid {}: {}", name, value), argv, usage);
  }
  return *parsed;
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

GenerateOptions
parse_generate_args(int argc, char *argv[])
{
  GenerateOptions options;
  std::optional<hogd::Ed25519PublicKey> faucet_public_key;
  std::optional<std::uint64_t> faucet_tokens;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(stdout, argv);
      std::exit(EXIT_SUCCESS);
    }
    if (arg == "--network-id" && i + 1 < argc) {
      options.config.network_id = argv[++i];
      if (options.config.network_id.empty()) {
        hogd::arg_error("Invalid network id: id must not be empty", argv,
                        usage);
      }
      continue;
    }
    if (arg == "--dev-wallet-profile" && i + 1 < argc) {
      options.config.bootstrap_config.dev_wallet_profile = argv[++i];
      if (options.config.bootstrap_config.dev_wallet_profile.empty()) {
        hogd::arg_error("Invalid dev wallet profile: profile must not be empty",
                        argv, usage);
      }
      hogd::resolve_dev_wallet_profile(
          options.config.bootstrap_config.dev_wallet_profile);
      continue;
    }
    if (arg == "--dev-accounts" && i + 1 < argc) {
      options.config.bootstrap_config.count =
          parse_bootstrap_account_count(argv[++i], argv);
      continue;
    }
    if (arg == "--dev-account-seed" && i + 1 < argc) {
      options.config.bootstrap_config.seed = argv[++i];
      if (options.config.bootstrap_config.seed.empty()) {
        hogd::arg_error("Invalid dev account seed: seed must not be empty",
                        argv, usage);
      }
      continue;
    }
    if (arg == "--dev-account-tokens" && i + 1 < argc) {
      const std::string_view value = argv[++i];
      std::optional<std::uint64_t> tokens = hogd::try_parse_u64(value);
      if (!tokens) {
        hogd::arg_error(std::format("Invalid dev account tokens: {}", value),
                        argv, usage);
      }
      options.config.bootstrap_config.storage_tokens = *tokens;
      continue;
    }
    if (arg == "--faucet-public-key" && i + 1 < argc) {
      if (faucet_public_key.has_value()) {
        hogd::arg_error("--faucet-public-key may only be specified once", argv,
                        usage);
      }
      faucet_public_key = parse_faucet_public_key(argv[++i], argv);
      continue;
    }
    if (arg == "--faucet-tokens" && i + 1 < argc) {
      if (faucet_tokens.has_value()) {
        hogd::arg_error("--faucet-tokens may only be specified once", argv,
                        usage);
      }
      const std::string_view value = argv[++i];
      faucet_tokens = hogd::try_parse_u64(value);
      if (!faucet_tokens) {
        hogd::arg_error(std::format("Invalid faucet tokens: {}", value), argv,
                        usage);
      }
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
        hogd::arg_error(
            "Invalid contract artifacts dir: path must not be empty", argv,
            usage);
      }
      options.contract_artifact_directories.push_back(std::move(dir));
      continue;
    }
    usage(argv);
  }

  if (options.genesis_out.empty()) {
    hogd::arg_error("--genesis-out is required", argv, usage);
  }
  if (options.metadata_out.empty()) {
    hogd::arg_error("--metadata-out is required", argv, usage);
  }
  if (faucet_public_key.has_value() != faucet_tokens.has_value()) {
    hogd::arg_error(
        "--faucet-public-key and --faucet-tokens must be specified together",
        argv, usage);
  }
  if (faucet_public_key.has_value()) {
    options.config.faucet = hogd::HostedFaucetConfig{
        .public_key = *faucet_public_key,
        .storage_tokens = *faucet_tokens,
    };
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
