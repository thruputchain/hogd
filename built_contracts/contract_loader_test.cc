#include "contract_loader.h"

#include <filesystem>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/log.h"
#include "x23/x23/progname.h"

namespace hogd {
namespace {

struct ContractArtifactDirectoryGuard {
  ~ContractArtifactDirectoryGuard() { set_contract_artifact_directories({}); }
};

TEST(ContractLoaderTest, SearchOrderIncludesUserDirsAndDefaults)
{
  ContractArtifactDirectoryGuard guard;
  std::vector<std::filesystem::path> user_dirs{
      "/tmp/groundhog-contracts-a",
      "/tmp/groundhog-contracts-b",
  };
  set_contract_artifact_directories(user_dirs);

  auto dirs = contract_search_directories();
  ASSERT_EQ(dirs.size(), 4U);
  EXPECT_EQ(dirs[0], user_dirs[0]);
  EXPECT_EQ(dirs[1], user_dirs[1]);
  EXPECT_EQ(dirs[2].filename(), "built_contracts");
  EXPECT_EQ(dirs[3], std::filesystem::path(x23::getprogpath()).parent_path() /
                         "contracts");
}

TEST(ContractLoaderTest, LoadsDevWalletFromBuildTree)
{
  ContractArtifactDirectoryGuard guard;

  auto rv32_blob = load_contract_blob("dev_wallet.rv32a");
  EXPECT_FALSE(rv32_blob.data.empty());
}

TEST(ContractLoaderTest, MissingArtifactNamesSearchPathsAndOverride)
{
  ContractArtifactDirectoryGuard guard;
  set_contract_artifact_directories({"/tmp/groundhog-missing-contracts"});

  EXPECT_DEATH(
      {
        log_init(std::make_unique<ConsoleLogger>());
        (void)load_contract_blob("missing-contract.rv32a");
      },
      "failed to load contract artifact: missing-contract\\.rv32a; searched "
      "directories: .*groundhog-missing-contracts.*--contract-artifacts-dir");
}

} // namespace
} // namespace hogd
