// In-process unit tests for hogd::load_bootstrap_manifest.
//
// These tests call the parser function directly and inspect the thrown
// exception's what() text. They are much faster than the subprocess-based
// tests in hog_devnode_test.cc because they avoid spawning hog-devnode.
//
// NOLINTNEXTLINE(misc-include-cleaner)
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "built_contracts/dev_wallet.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/json.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/bootstrap_manifest.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/init.h"
#include "hogd/version.h"

namespace hogd {
namespace {

using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

// Returns the SHA-256 hex of the bundled dev-wallet artifact. Used as the
// "correct" devWallet.contentHash value in happy-path fixtures.
std::string
real_dev_wallet_content_hash()
{
  auto wallet_contract = load_dev_wallet_contract(
      resolve_dev_wallet_profile(kDefaultDevWalletProfile));
  if (!wallet_contract) {
    throw std::runtime_error("dev_wallet contract failed to load");
  }
  return hex_string(sha256(*wallet_contract));
}

// Builds a minimal valid bootstrap manifest JSON document. Each builder
// option corresponds to one field; the defaults represent a well-formed
// manifest. Tests tweak only the field(s) under test.
struct ManifestBuilder {
  bool include_schema_version = true;
  std::int64_t schema_version = 1;

  std::string kind = "groundhog-bootstrap";

  bool include_network_id = true;
  std::string network_id = "groundhog-bench-local";

  std::string profile = "local-dev";

  bool include_protocol = true;
  std::string protocol = std::string(version::kHogdProtocol());

  bool include_dev_wallet = true;
  std::string dev_wallet_profile = "hog-devnode-default";
  bool include_dev_wallet_content_hash = true;
  std::string dev_wallet_content_hash;
  std::string dev_wallet_deploy_entrypoint;
  std::string dev_wallet_do_action_entrypoint;

  bool include_bootstrap_accounts = true;
  std::int64_t bootstrap_count = 2;
  bool include_bootstrap_seed = true;
  std::string bootstrap_seed = "manifest-seed";
  std::string initial_storage_tokens = "1000000000";

  bool include_storage_cost_model = true;
  std::string storage_cost_profile = "hogd-default";

  ManifestBuilder()
  {
    dev_wallet_content_hash = real_dev_wallet_content_hash();
    dev_wallet_deploy_entrypoint =
        entrypoint_hex(wallet_meta::entrypoint<"initialize">());
    dev_wallet_do_action_entrypoint =
        entrypoint_hex(wallet_meta::entrypoint<"do_action">());
  }

  std::string build() const
  {
    std::string buffer;
    json::Writer(buffer).object([&](json::Writer &w) {
      if (include_schema_version) {
        w.field("schemaVersion", schema_version);
      }
      w.field("kind", kind);
      if (include_network_id) {
        w.field("networkId", network_id);
      }
      w.field("profile", profile);
      if (include_protocol) {
        w.field("protocol", protocol);
      }
      if (include_dev_wallet) {
        w.object_field("devWallet", [&](json::Writer &w) {
          w.field("profile", dev_wallet_profile);
          w.field("artifactHint", "./dev-wallet.rv32a");
          if (include_dev_wallet_content_hash) {
            w.field("contentHash", dev_wallet_content_hash);
          }
          w.field("deployEntrypoint", dev_wallet_deploy_entrypoint);
          w.field("doActionEntrypoint", dev_wallet_do_action_entrypoint);
        });
      }
      if (include_bootstrap_accounts) {
        w.object_field("bootstrapAccounts", [&](json::Writer &w) {
          w.field("count", bootstrap_count);
          if (include_bootstrap_seed) {
            w.field("seed", bootstrap_seed);
          }
          w.field("initialStorageTokens", initial_storage_tokens);
        });
      }
      if (include_storage_cost_model) {
        w.object_field("storageCostModel", [&](json::Writer &w) {
          w.field("profile", storage_cost_profile);
        });
      }
    });
    return buffer;
  }
};

// Writes `contents` to a fresh temp file and returns the path. Pattern
// mirrors hog_devnode_test.cc's write_test_bootstrap_manifest helper.
std::filesystem::path
write_temp_manifest(std::string_view contents)
{
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-unit-{}-{}.json", getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error(std::format("failed to write {}", path.string()));
  }
  output << contents;
  return path;
}

// Writes contents to a temp file and registers a defer to remove it.
// Returns the path. Same call-site pattern as hog_devnode_test.cc.
// `cleanup` is an erased Defer<std::function<void()>> so the helper can
// assign into it (Defer<lambda> would be a different specialisation).
using CleanupDefer = Defer<std::function<void()>>;

std::filesystem::path
write_temp_manifest_with_cleanup(std::string_view contents,
                                 CleanupDefer &cleanup)
{
  std::filesystem::path path = write_temp_manifest(contents);
  cleanup = CleanupDefer([path] {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  });
  return path;
}

// Asserts that calling load_bootstrap_manifest(path) throws a runtime_error
// whose what() contains `needle`. Match the existing find()-based pattern.
void
expect_throws_with(std::filesystem::path const &path, std::string_view needle)
{
  try {
    (void)load_bootstrap_manifest(path);
    ADD_FAILURE() << "expected load_bootstrap_manifest to throw (needle: "
                  << needle << ")";
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
        << "actual: " << e.what() << "\nneedle: " << needle;
  }
}

TEST(BootstrapManifest, LoadBootstrapManifestHappyPath)
{
  ManifestBuilder builder;
  builder.network_id = "groundhog-bench-local";
  builder.bootstrap_count = 2;
  builder.bootstrap_seed = "manifest-seed";
  builder.initial_storage_tokens = "1000000000";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);

  BootstrapManifestConfig config = load_bootstrap_manifest(path);
  EXPECT_EQ(config.network_id, "groundhog-bench-local");
  EXPECT_EQ(config.bootstrap_config.count, 2U);
  EXPECT_EQ(config.bootstrap_config.seed, "manifest-seed");
  EXPECT_EQ(config.bootstrap_config.storage_tokens, 1'000'000'000ULL);
}

TEST(BootstrapManifest, RejectsNonObjectRoot)
{
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup("[1, 2, 3]", cleanup);
  expect_throws_with(path, "root is not an object");
}

TEST(BootstrapManifest, RejectsInvalidJson)
{
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup("{not-json", cleanup);
  expect_throws_with(path, "JSON parse failed");
}

TEST(BootstrapManifest, RejectsWrongSchemaVersion)
{
  ManifestBuilder builder;
  builder.schema_version = 2;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "schemaVersion");
}

TEST(BootstrapManifest, RejectsMissingSchemaVersion)
{
  ManifestBuilder builder;
  builder.include_schema_version = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "missing root.schemaVersion");
}

TEST(BootstrapManifest, RejectsWrongKind)
{
  ManifestBuilder builder;
  builder.kind = "something-else";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "kind");
}

TEST(BootstrapManifest, RejectsWrongProfile)
{
  ManifestBuilder builder;
  builder.profile = "production";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "profile");
}

TEST(BootstrapManifest, RejectsProtocolMismatch)
{
  ManifestBuilder builder;
  builder.protocol = "hogp/wrong-thing";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "protocol");
}

TEST(BootstrapManifest, AcceptsAbsentProtocol)
{
  ManifestBuilder builder;
  builder.include_protocol = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  EXPECT_NO_THROW((void)load_bootstrap_manifest(path));
}

TEST(BootstrapManifest, RejectsEmptyNetworkId)
{
  ManifestBuilder builder;
  builder.network_id = "";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "networkId must not be empty");
}

TEST(BootstrapManifest, RejectsMissingNetworkId)
{
  ManifestBuilder builder;
  builder.include_network_id = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "missing root.networkId");
}

TEST(BootstrapManifest, RejectsMissingDevWallet)
{
  ManifestBuilder builder;
  builder.include_dev_wallet = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "missing root.devWallet");
}

TEST(BootstrapManifest, RejectsWrongDevWalletProfile)
{
  ManifestBuilder builder;
  builder.dev_wallet_profile = "wrong";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "devWallet.profile");
}

TEST(BootstrapManifest, RejectsMismatchedContentHash)
{
  ManifestBuilder builder;
  builder.dev_wallet_content_hash =
      "0x9999999999999999999999999999999999999999999999999999999999999999";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "devWallet.contentHash");
}

TEST(BootstrapManifest, RejectsMismatchedDeployEntrypoint)
{
  ManifestBuilder builder;
  builder.dev_wallet_deploy_entrypoint = "0xdeadbeefdeadbeef";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "devWallet.deployEntrypoint");
}

TEST(BootstrapManifest, RejectsMismatchedDoActionEntrypoint)
{
  ManifestBuilder builder;
  builder.dev_wallet_do_action_entrypoint = "0xdeadbeefdeadbeef";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "devWallet.doActionEntrypoint");
}

TEST(BootstrapManifest, AcceptsOmittedContentHash)
{
  ManifestBuilder builder;
  builder.include_dev_wallet_content_hash = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  EXPECT_NO_THROW((void)load_bootstrap_manifest(path));
}

TEST(BootstrapManifest, RejectsMissingBootstrapAccounts)
{
  ManifestBuilder builder;
  builder.include_bootstrap_accounts = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "missing root.bootstrapAccounts");
}

TEST(BootstrapManifest, RejectsNegativeCount)
{
  ManifestBuilder builder;
  builder.bootstrap_count = -1;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "bootstrapAccounts.count");
}

TEST(BootstrapManifest, RejectsCountAboveLimit)
{
  ManifestBuilder builder;
  builder.bootstrap_count = 100000;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "maximum");
}

TEST(BootstrapManifest, RejectsEmptySeed)
{
  ManifestBuilder builder;
  builder.bootstrap_seed = "";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "bootstrapAccounts.seed must not be empty");
}

TEST(BootstrapManifest, RejectsMissingSeed)
{
  ManifestBuilder builder;
  builder.include_bootstrap_seed = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "missing bootstrapAccounts.seed");
}

TEST(BootstrapManifest, RejectsMissingStorageCostModel)
{
  ManifestBuilder builder;
  builder.include_storage_cost_model = false;
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "missing root.storageCostModel");
}

TEST(BootstrapManifest, RejectsWrongStorageCostModelProfile)
{
  ManifestBuilder builder;
  builder.storage_cost_profile = "wrong";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "storageCostModel.profile");
}

TEST(BootstrapManifest, RejectsInitialStorageTokensWithLeadingWhitespace)
{
  ManifestBuilder builder;
  builder.initial_storage_tokens = " 1000";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "initialStorageTokens");
}

TEST(BootstrapManifest, RejectsInitialStorageTokensWithPlusSign)
{
  ManifestBuilder builder;
  builder.initial_storage_tokens = "+1000";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "initialStorageTokens");
}

TEST(BootstrapManifest, RejectsInitialStorageTokensEmpty)
{
  ManifestBuilder builder;
  builder.initial_storage_tokens = "";
  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(builder.build(), cleanup);
  expect_throws_with(path, "initialStorageTokens");
}

TEST(BootstrapManifest, RejectsMissingFile)
{
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-missing-{}-{}.json", getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  // Ensure the file truly does not exist.
  std::filesystem::remove(path);
  expect_throws_with(path, path.string());
}

TEST(BootstrapManifest, RejectsDirectoryAsManifest)
{
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-dir-{}-{}", getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(dir);
  Defer cleanup([dir] { std::filesystem::remove_all(dir); });
  expect_throws_with(dir, "must be a regular file");
}

TEST(BootstrapManifest, RejectsOversizedManifest)
{
  CleanupDefer cleanup;
  // 64 KiB + 1 byte, all whitespace -- read_bootstrap_manifest_file rejects
  // before JSON parsing.
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(std::string((64 * 1024) + 1, ' '),
                                       cleanup);
  expect_throws_with(path, "exceeds");
}

TEST(BootstrapManifest, AcceptsManifestAtExactSizeLimit)
{
  ManifestBuilder builder;
  std::string manifest = builder.build();
  ASSERT_LT(manifest.size(), 64UL * 1024UL);
  manifest.resize(64UL * 1024UL, ' ');

  CleanupDefer cleanup;
  std::filesystem::path path =
      write_temp_manifest_with_cleanup(manifest, cleanup);
  EXPECT_NO_THROW((void)load_bootstrap_manifest(path));
}

// A dangling symlink (target does not exist) is a regression test for the
// O_NOFOLLOW switch: under the previous stat-then-open implementation,
// symlink_status() would succeed on the link itself and the failure mode
// would be "failed to stat" (well, it actually reported the link as a
// symlink, so the message would still be "must be a regular file"; the
// real bite was the TOCTOU window between stat and open). With the
// O_NOFOLLOW open, the very first syscall fails with ELOOP, which we map
// to "must be a regular file" -- the existing assertion needle still
// fires, but now via a single race-free syscall.
TEST(BootstrapManifest, RejectsDanglingSymlink)
{
  std::filesystem::path missing_target =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-missing-target-{}-{}.json",
                  getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  // Make doubly sure the target does not exist.
  std::error_code ec;
  std::filesystem::remove(missing_target, ec);

  std::filesystem::path symlink_path =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-dangling-{}-{}.json",
                  getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_symlink(missing_target, symlink_path);
  Defer link_cleanup([symlink_path] {
    std::error_code ec;
    std::filesystem::remove(symlink_path, ec);
  });

  expect_throws_with(symlink_path, "must be a regular file");
}

// /dev/null is a character device: open() succeeds without follow, but
// fstat() reports S_ISCHR, so the explicit S_ISREG check on the fd rejects
// it. This exercises the post-open inode check that the stat-then-open
// implementation also had but at a different layer.
TEST(BootstrapManifest, RejectsCharacterDeviceAsManifest)
{
  expect_throws_with("/dev/null", "must be a regular file");
}

TEST(BootstrapManifest, RejectsSymlink)
{
  ManifestBuilder builder;
  CleanupDefer file_cleanup;
  std::filesystem::path real_path =
      write_temp_manifest_with_cleanup(builder.build(), file_cleanup);

  std::filesystem::path symlink_path =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-symlink-{}-{}.json", getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_symlink(real_path, symlink_path);
  Defer link_cleanup([symlink_path] {
    std::error_code ec;
    std::filesystem::remove(symlink_path, ec);
  });

  expect_throws_with(symlink_path, "must be a regular file");
}

} // namespace
} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0) {
    return ret;
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
