#include "contract_loader.h"

#include <algorithm>
#include <filesystem>
#include <string_view>
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

TEST(ContractLoaderTest, EmbeddedArtifactServedBeforeDiskSearch)
{
  // A name that exists in no search directory: without the registration the
  // load would panic, so a successful load proves the registry served it.
  constexpr std::string_view kName = "embedded-only-test-artifact.rv32a";
  static constexpr unsigned char kBytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
  register_embedded_contract_artifact(kName, kBytes);

  auto blob = load_contract_blob(kName);
  ASSERT_EQ(blob.data.size(), sizeof(kBytes));
  EXPECT_TRUE(
      std::equal(blob.data.begin(), blob.data.end(), std::begin(kBytes)));

  // Re-registration replaces the previous bytes.
  static constexpr unsigned char kReplaced[] = {0x01};
  register_embedded_contract_artifact(kName, kReplaced);
  auto replaced = load_contract_blob(kName);
  ASSERT_EQ(replaced.data.size(), sizeof(kReplaced));
  EXPECT_EQ(replaced.data[0], 0x01);
}

} // namespace
} // namespace hogd
