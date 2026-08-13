#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "contract.hh"
#include "hogd/base/cleanup.h"
#include "hogd/base/json.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/hosted_bootstrap_generator.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/init.h"
#include "types.hh"

#ifndef HOG_HOSTED_BOOTSTRAP_BINARY
#error "HOG_HOSTED_BOOTSTRAP_BINARY must be defined by CMake"
#endif

namespace hogd {
namespace {

const json::Value &
require_field(json::Value const &parent, std::string_view key)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(std::format("missing field {}", key));
  }
  return *field;
}

std::string_view
require_string(json::Value const &value)
{
  std::optional<std::string_view> string = value.as_string();
  if (!string.has_value()) {
    throw std::runtime_error("expected JSON string");
  }
  return *string;
}

std::filesystem::path
unique_test_dir(std::string_view name)
{
  return std::filesystem::temp_directory_path() /
         std::format(
             "hog-hosted-bootstrap-{}-{}-{}", name, getpid(),
             std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string
read_text_file(std::filesystem::path const &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(std::format("failed to read {}", path.string()));
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

int
run_hosted_bootstrap(std::vector<std::string> args)
{
  args.insert(args.begin(), HOG_HOSTED_BOOTSTRAP_BINARY);
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (std::string &arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const auto pid = fork();
  if (pid < 0) {
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    execv(HOG_HOSTED_BOOTSTRAP_BINARY, argv.data());
    _exit(127);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error("waitpid failed");
    }
  }
  return status;
}

TEST(HostedBootstrapGenerator, GeneratesGenesisAndMetadata)
{
  HostedBootstrapGeneratorConfig config{
      .network_hash =
          [] {
            NetworkHash hash{};
            hash.back() = 1;
            return hash;
          }(),
      .bootstrap_config =
          BootstrapAccountConfig{
              .count = 2,
              .seed = "hosted-test-seed",
              .storage_tokens = 12345,
              .dev_wallet_profile = std::string(kDefaultDevWalletProfile),
          },
  };

  HostedBootstrapArtifact artifact = build_hosted_bootstrap_artifact(config);

  EXPECT_EQ(artifact.accounts.size(), 2UL);
  EXPECT_EQ(artifact.initialization_txs.size(), 2UL);
  EXPECT_EQ(artifact.genesis_configuration.initial_config.size(), 2UL);

  std::expected<json::Value, json::ParseError> genesis =
      json::parse(artifact.genesis_json);
  ASSERT_TRUE(genesis.has_value()) << genesis.error().message;
  ASSERT_EQ(require_field(*genesis, "initial_config").as_array().size(), 2UL);
  const json::Value &first_config = require_field(
      require_field(require_field(*genesis, "initial_config").as_array()[0],
                    "config"),
      "contract_hash");
  std::string_view first_config_string = require_string(first_config);
  EXPECT_EQ(first_config_string.size(), 64UL);
  EXPECT_FALSE(first_config_string.starts_with("0x"));
  EXPECT_EQ(require_string(
                require_field(require_field(*genesis, "storage_cost_model"),
                              "bytestring_cost_per_byte")),
            "10");
  const json::Value &default_wallet = require_field(*genesis, "default_wallet");
  // Genesis-JSON hashes are unprefixed hex (see contract_hash above);
  // hex_string prepends 0x.
  EXPECT_EQ(std::format("0x{}",
                        require_string(require_field(default_wallet, "hash"))),
            hex_string(
                GenesisConfigurationLoader::native_default_wallet_hash().hash));
  EXPECT_EQ(require_field(default_wallet, "contract_size").as_integer(),
            std::optional<std::int64_t>{0});
  EXPECT_EQ(require_field(default_wallet, "contract_type").as_integer(),
            std::optional<std::int64_t>{
                static_cast<std::int64_t>(ContractType::kNative)});

  std::expected<json::Value, json::ParseError> metadata =
      json::parse(artifact.metadata_json);
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message;
  std::optional<std::int64_t> schema_version =
      require_field(*metadata, "schemaVersion").as_integer();
  EXPECT_EQ(schema_version, std::optional<std::int64_t>{1});
  EXPECT_EQ(require_string(require_field(*metadata, "kind")),
            "hog-hosted-bootstrap");
  EXPECT_EQ(require_string(require_field(*metadata, "networkHash")),
            "0000000000000000000000000000000000000000000000000000000000000001");
  EXPECT_EQ(require_string(require_field(*metadata, "genesisHash")),
            hex_string(artifact.genesis_hash));
  EXPECT_EQ(require_field(*metadata, "bootstrapAccounts").as_array().size(),
            2UL);
  for (const json::Value &account :
       require_field(*metadata, "bootstrapAccounts").as_array()) {
    EXPECT_EQ(require_string(require_field(account, "role")),
              "hosted_dev_account");
  }
  EXPECT_EQ(metadata->find("faucet"), nullptr);
  std::span<const json::Value> initialization_txs =
      require_field(*metadata, "initializationTransactions").as_array();
  ASSERT_EQ(initialization_txs.size(), 2UL);
  EXPECT_TRUE(require_string(require_field(initialization_txs[0], "hash"))
                  .starts_with("0x"));
  EXPECT_FALSE(require_string(require_field(initialization_txs[0], "xdr"))
                   .starts_with("0x"));
}

TEST(HostedBootstrapGenerator, DefaultRemainsHomogeneousWithoutFaucet)
{
  HostedBootstrapGeneratorConfig config;
  HostedBootstrapArtifact artifact = build_hosted_bootstrap_artifact(config);

  ASSERT_EQ(artifact.accounts.size(), 10UL);
  for (std::uint32_t index = 0; index < artifact.accounts.size(); ++index) {
    EXPECT_EQ(artifact.accounts[index].index, index);
    EXPECT_EQ(artifact.accounts[index].role,
              BootstrapAccountRole::kHostedDevAccount);
    EXPECT_EQ(artifact.accounts[index].initial_storage_tokens,
              1'000'000'000ULL);
  }
  EXPECT_FALSE(artifact.faucet.has_value());

  std::expected<json::Value, json::ParseError> metadata =
      json::parse(artifact.metadata_json);
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message;
  EXPECT_EQ(metadata->find("faucet"), nullptr);
}

TEST(HostedBootstrapGenerator, GeneratesExplicitFaucetWallet)
{
  const Ed25519PublicKey faucet_public_key =
      derive_bootstrap_account_keypair("faucet-test-seed", 0).first;
  HostedBootstrapGeneratorConfig config;
  config.bootstrap_config.count = 2;
  config.faucet = HostedFaucetConfig{
      .public_key = faucet_public_key,
      .storage_tokens = 987654321,
  };

  HostedBootstrapArtifact first = build_hosted_bootstrap_artifact(config);
  HostedBootstrapArtifact second = build_hosted_bootstrap_artifact(config);

  if (!first.faucet.has_value() || !second.faucet.has_value()) {
    ADD_FAILURE() << "expected faucet metadata";
    return;
  }
  HostedFaucetMetadata const &first_faucet = first.faucet.value();
  HostedFaucetMetadata const &second_faucet = second.faucet.value();
  EXPECT_EQ(first.accounts.size(), 2UL);
  EXPECT_EQ(first.initialization_txs.size(), 3UL);
  EXPECT_EQ(first.genesis_configuration.initial_config.size(), 3UL);
  EXPECT_EQ(first_faucet.public_key, faucet_public_key);
  EXPECT_EQ(first_faucet.initial_storage_tokens, 987654321UL);
  EXPECT_EQ(second_faucet.wallet_address, first_faucet.wallet_address);
  EXPECT_EQ(second.genesis_json, first.genesis_json);
  EXPECT_EQ(second.metadata_json, first.metadata_json);

  std::expected<json::Value, json::ParseError> metadata =
      json::parse(first.metadata_json);
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message;
  const json::Value &faucet = require_field(*metadata, "faucet");
  EXPECT_EQ(require_string(require_field(faucet, "role")), "faucet");
  EXPECT_EQ(require_string(require_field(faucet, "publicKey")),
            hex_string(faucet_public_key));
  EXPECT_EQ(require_string(require_field(faucet, "walletAddress")),
            hex_string(first_faucet.wallet_address));
  EXPECT_EQ(require_string(require_field(faucet, "initialStorageTokens")),
            "987654321");
  EXPECT_EQ(require_field(*metadata, "bootstrapAccounts").as_array().size(),
            2UL);
}

TEST(HostedBootstrapGenerator, RejectsInvalidFaucetAllocation)
{
  HostedBootstrapGeneratorConfig config;
  config.faucet = HostedFaucetConfig{
      .public_key =
          derive_bootstrap_account_keypair("faucet-test-seed", 0).first,
      .storage_tokens = 0,
  };
  EXPECT_THROW(build_hosted_bootstrap_artifact(config), std::runtime_error);

  config.faucet->storage_tokens = 1;
  config.faucet->public_key = {};
  EXPECT_THROW(build_hosted_bootstrap_artifact(config), std::runtime_error);

  config.faucet->public_key =
      derive_bootstrap_account_keypair(config.bootstrap_config.seed, 0).first;
  EXPECT_THROW(build_hosted_bootstrap_artifact(config), std::runtime_error);
}

TEST(HostedBootstrapGenerator,
     RejectsSeedDerivedFaucetKeyOutsideAllocatedAccounts)
{
  HostedBootstrapGeneratorConfig config;
  config.bootstrap_config.count = 1;
  config.faucet = HostedFaucetConfig{
      .public_key =
          derive_bootstrap_account_keypair(config.bootstrap_config.seed, 1)
              .first,
      .storage_tokens = 1,
  };
  EXPECT_THROW(build_hosted_bootstrap_artifact(config), std::runtime_error);

  config.bootstrap_config.count = 0;
  config.faucet->public_key =
      derive_bootstrap_account_keypair(config.bootstrap_config.seed,
                                       kMaxBootstrapAccounts - 1)
          .first;
  EXPECT_THROW(build_hosted_bootstrap_artifact(config), std::runtime_error);
}

TEST(HostedBootstrapGenerator, CliRequiresCompleteFaucetConfiguration)
{
  std::filesystem::path dir = unique_test_dir("cli-partial");
  Defer cleanup([&] {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  });
  const Ed25519PublicKey faucet_public_key =
      derive_bootstrap_account_keypair("faucet-cli-test-seed", 0).first;
  const int status = run_hosted_bootstrap({
      "generate",
      "--faucet-public-key",
      hex_string(faucet_public_key),
      "--genesis-out",
      (dir / "genesis.json").string(),
      "--metadata-out",
      (dir / "metadata.json").string(),
  });

  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_NE(WEXITSTATUS(status), EXIT_SUCCESS);
  EXPECT_FALSE(std::filesystem::exists(dir / "genesis.json"));
  EXPECT_FALSE(std::filesystem::exists(dir / "metadata.json"));
}

TEST(HostedBootstrapGenerator, CliRejectsMalformedFaucetPublicKey)
{
  std::filesystem::path dir = unique_test_dir("cli-invalid-key");
  Defer cleanup([&] {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  });
  const int status = run_hosted_bootstrap({
      "generate",
      "--faucet-public-key",
      std::string(64, 'g'),
      "--faucet-tokens",
      "456789",
      "--genesis-out",
      (dir / "genesis.json").string(),
      "--metadata-out",
      (dir / "metadata.json").string(),
  });

  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_NE(WEXITSTATUS(status), EXIT_SUCCESS);
  EXPECT_FALSE(std::filesystem::exists(dir / "genesis.json"));
  EXPECT_FALSE(std::filesystem::exists(dir / "metadata.json"));
}

TEST(HostedBootstrapGenerator, CliGeneratesExplicitFaucetWallet)
{
  std::filesystem::path dir = unique_test_dir("cli-faucet");
  Defer cleanup([&] {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  });
  const Ed25519PublicKey faucet_public_key =
      derive_bootstrap_account_keypair("faucet-cli-test-seed", 0).first;
  const std::vector<std::string> args{
      "generate",
      "--dev-accounts",
      "1",
      "--faucet-public-key",
      hex_string(faucet_public_key),
      "--faucet-tokens",
      "456789",
      "--genesis-out",
      (dir / "genesis.json").string(),
      "--metadata-out",
      (dir / "metadata.json").string(),
  };
  const int status = run_hosted_bootstrap(args);

  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
  const std::string first_genesis = read_text_file(dir / "genesis.json");
  const std::string first_metadata = read_text_file(dir / "metadata.json");

  const int rerun_status = run_hosted_bootstrap(args);
  ASSERT_TRUE(WIFEXITED(rerun_status));
  ASSERT_EQ(WEXITSTATUS(rerun_status), EXIT_SUCCESS);
  EXPECT_EQ(read_text_file(dir / "genesis.json"), first_genesis);
  EXPECT_EQ(read_text_file(dir / "metadata.json"), first_metadata);

  std::expected<json::Value, json::ParseError> metadata =
      json::parse(first_metadata);
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message;
  EXPECT_EQ(require_string(
                require_field(require_field(*metadata, "faucet"), "publicKey")),
            hex_string(faucet_public_key));
}

TEST(HostedBootstrapGenerator, AllowsZeroBootstrapAccounts)
{
  HostedBootstrapGeneratorConfig config;
  config.bootstrap_config.count = 0;

  HostedBootstrapArtifact artifact = build_hosted_bootstrap_artifact(config);

  EXPECT_TRUE(artifact.accounts.empty());
  EXPECT_TRUE(artifact.initialization_txs.empty());
  EXPECT_TRUE(artifact.genesis_configuration.initial_config.empty());

  std::expected<json::Value, json::ParseError> metadata =
      json::parse(artifact.metadata_json);
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message;
  EXPECT_EQ(require_field(*metadata, "bootstrapAccounts").as_array().size(),
            0UL);
}

TEST(HostedBootstrapGenerator, WriteArtifactAllowsIdenticalRerun)
{
  std::filesystem::path dir = unique_test_dir("same");
  Defer cleanup([&] {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  });
  std::filesystem::path genesis = dir / "nested" / "genesis.json";
  std::filesystem::path metadata = dir / "nested" / "metadata.json";

  HostedBootstrapGeneratorConfig config;
  config.bootstrap_config.count = 1;

  write_hosted_bootstrap_artifact(config, genesis, metadata);
  std::string first_genesis = read_text_file(genesis);
  std::string first_metadata = read_text_file(metadata);
  write_hosted_bootstrap_artifact(config, genesis, metadata);

  EXPECT_EQ(read_text_file(genesis), first_genesis);
  EXPECT_EQ(read_text_file(metadata), first_metadata);
}

TEST(HostedBootstrapGenerator, WriteArtifactRejectsExistingEmptyFile)
{
  std::filesystem::path dir = unique_test_dir("empty");
  Defer cleanup([&] {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  });
  std::filesystem::create_directories(dir);
  std::filesystem::path genesis = dir / "genesis.json";
  std::filesystem::path metadata = dir / "metadata.json";
  {
    std::ofstream output(genesis, std::ios::binary);
    ASSERT_TRUE(output.good());
  }

  HostedBootstrapGeneratorConfig config;
  config.bootstrap_config.count = 1;

  EXPECT_THROW(write_hosted_bootstrap_artifact(config, genesis, metadata),
               std::runtime_error);
}

TEST(HostedBootstrapGenerator, WriteArtifactDoesNotClobberTmpLikeOutputPath)
{
  std::filesystem::path dir = unique_test_dir("tmp-collision");
  Defer cleanup([&] {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  });
  std::filesystem::path genesis = dir / "metadata.json.tmp";
  std::filesystem::path metadata = dir / "metadata.json";

  HostedBootstrapGeneratorConfig config;
  config.bootstrap_config.count = 1;

  write_hosted_bootstrap_artifact(config, genesis, metadata);

  EXPECT_TRUE(std::filesystem::exists(genesis));
  EXPECT_TRUE(std::filesystem::exists(metadata));
  EXPECT_NE(read_text_file(genesis), read_text_file(metadata));
}

} // namespace
} // namespace hogd

int
main(int argc, char **argv)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
