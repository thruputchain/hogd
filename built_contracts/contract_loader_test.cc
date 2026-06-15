#include "contract_loader.h"

#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

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
  ASSERT_EQ(dirs.size(), 5u);
  EXPECT_EQ(dirs[0], user_dirs[0]);
  EXPECT_EQ(dirs[1], user_dirs[1]);
  EXPECT_EQ(dirs[2].filename(), "built_contracts");
  EXPECT_EQ(dirs[3], std::filesystem::path(x23::getprogpath()).parent_path() /
                         "contracts");
  EXPECT_EQ(dirs[4], std::filesystem::path("/usr/share/groundhog/contracts"));
}

TEST(ContractLoaderTest, LoadsDevWalletFromBuildTree)
{
  ContractArtifactDirectoryGuard guard;
  auto blob = load_contract_blob("dev_wallet.wasm");
  EXPECT_FALSE(blob.data.empty());

  auto rv32_blob = load_contract_blob("dev_wallet.rv32a");
  EXPECT_FALSE(rv32_blob.data.empty());
}

} // namespace
} // namespace hogd
