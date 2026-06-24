#include <cstdint>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

#include "hogd/base/cleanup.h"
#include "hogd/base/json.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/hosted_bootstrap_generator.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/init.h"

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

std::filesystem::path
unique_test_dir(std::string_view name)
{
  return std::filesystem::temp_directory_path() /
         std::format("hog-hosted-bootstrap-{}-{}-{}", name, getpid(),
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count());
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

TEST(HostedBootstrapGenerator, GeneratesGenesisAndMetadata)
{
  HostedBootstrapGeneratorConfig config{
      .network_id = "devnet-test",
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
  ASSERT_TRUE(first_config.as_string().has_value());
  EXPECT_EQ(first_config.as_string()->size(), 64UL);
  EXPECT_FALSE(first_config.as_string()->starts_with("0x"));

  std::expected<json::Value, json::ParseError> metadata =
      json::parse(artifact.metadata_json);
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message;
  ASSERT_TRUE(require_field(*metadata, "kind").as_string().has_value());
  EXPECT_EQ(*require_field(*metadata, "kind").as_string(),
            "hog-hosted-bootstrap");
  ASSERT_TRUE(require_field(*metadata, "networkId").as_string().has_value());
  EXPECT_EQ(*require_field(*metadata, "networkId").as_string(), "devnet-test");
  ASSERT_TRUE(require_field(*metadata, "genesisHash").as_string().has_value());
  EXPECT_EQ(*require_field(*metadata, "genesisHash").as_string(),
            hex_string(artifact.genesis_hash));
  EXPECT_EQ(require_field(*metadata, "bootstrapAccounts").as_array().size(),
            2UL);
  std::span<const json::Value> initialization_txs =
      require_field(*metadata, "initializationTransactions").as_array();
  ASSERT_EQ(initialization_txs.size(), 2UL);
  ASSERT_TRUE(
      require_field(initialization_txs[0], "hash").as_string().has_value());
  ASSERT_TRUE(
      require_field(initialization_txs[0], "xdr").as_string().has_value());
  EXPECT_TRUE(require_field(initialization_txs[0], "hash")
                  .as_string()
                  ->starts_with("0x"));
  EXPECT_FALSE(require_field(initialization_txs[0], "xdr")
                   .as_string()
                   ->starts_with("0x"));
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
