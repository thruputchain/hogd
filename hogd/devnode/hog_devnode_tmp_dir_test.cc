#include "hogd/devnode/hog_devnode_tmp_dir.h"

// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace hogd {
namespace {

// scratch_path returns a path under the system temp dir that no other test run
// shares. Nothing is created at it.
std::filesystem::path
scratch_path(std::string_view label)
{
  return std::filesystem::temp_directory_path() /
         std::format(
             "hog-devnode-tmp-dir-{}-{}-{}", label, getpid(),
             std::chrono::steady_clock::now().time_since_epoch().count());
}

// TmpdirOverride points $TMPDIR at [path] for as long as it is alive, so a test
// can drive the temp dir failure path without touching the real one.
class TmpdirOverride {
public:
  explicit TmpdirOverride(std::string const &path)
  {
    if (const char *previous = getenv("TMPDIR"); previous != nullptr) {
      previous_ = previous;
    }
    setenv("TMPDIR", path.c_str(), /*overwrite=*/1);
  }

  ~TmpdirOverride()
  {
    if (previous_.has_value()) {
      setenv("TMPDIR", previous_->c_str(), /*overwrite=*/1);
      return;
    }
    unsetenv("TMPDIR");
  }

  TmpdirOverride(TmpdirOverride const &) = delete;
  TmpdirOverride &operator=(TmpdirOverride const &) = delete;
  TmpdirOverride(TmpdirOverride &&) = delete;
  TmpdirOverride &operator=(TmpdirOverride &&) = delete;

private:
  std::optional<std::string> previous_;
};

TEST(HogDevnodeTmpDir, TemporaryDirectoryIsRemovedOnDestruction)
{
  std::string path;
  {
    std::expected<HogDevnodeTmpDir, std::string> dir =
        HogDevnodeTmpDir::create(std::nullopt, "runtime");
    ASSERT_TRUE(dir.has_value()) << dir.error();
    path = dir->directory();
    EXPECT_TRUE(std::filesystem::is_directory(path));
    EXPECT_TRUE(std::filesystem::path(path).is_absolute());
  }
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(HogDevnodeTmpDir, DataDirSubdirectoryOutlivesTheHandle)
{
  const std::filesystem::path data_dir = scratch_path("data");
  std::filesystem::create_directories(data_dir);
  std::string path;
  {
    std::expected<HogDevnodeTmpDir, std::string> dir =
        HogDevnodeTmpDir::create(data_dir, "runtime");
    ASSERT_TRUE(dir.has_value()) << dir.error();
    path = dir->directory();
    EXPECT_EQ(path, std::filesystem::absolute(data_dir / "runtime").string());
    EXPECT_TRUE(std::filesystem::is_directory(path));
  }
  EXPECT_TRUE(std::filesystem::is_directory(path));

  std::filesystem::remove_all(data_dir);
}

TEST(HogDevnodeTmpDir, UnusableTmpdirIsReportedAsAnError)
{
  const std::filesystem::path missing = scratch_path("missing");
  TmpdirOverride tmpdir(missing.string() + "/");

  std::expected<HogDevnodeTmpDir, std::string> dir =
      HogDevnodeTmpDir::create(std::nullopt, "runtime");
  ASSERT_FALSE(dir.has_value()) << dir->directory();
  EXPECT_TRUE(dir.error().contains("could not create a hog-devnode temp dir"))
      << dir.error();
  EXPECT_TRUE(dir.error().contains("TMPDIR")) << dir.error();
}

TEST(HogDevnodeTmpDir, DataPathThatIsAFileIsReportedAsAnError)
{
  const std::filesystem::path data_dir = scratch_path("file-data");
  std::filesystem::create_directories(data_dir);
  {
    std::ofstream(data_dir / "runtime") << "not a directory";
  }

  std::expected<HogDevnodeTmpDir, std::string> dir =
      HogDevnodeTmpDir::create(data_dir, "runtime");
  ASSERT_FALSE(dir.has_value()) << dir->directory();
  EXPECT_TRUE(dir.error().contains((data_dir / "runtime").string()))
      << dir.error();

  std::filesystem::remove_all(data_dir);
}

TEST(HogDevnodeTmpDir, MovingHandsOverTheDutyToRemove)
{
  std::expected<HogDevnodeTmpDir, std::string> dir =
      HogDevnodeTmpDir::create(std::nullopt, "runtime");
  ASSERT_TRUE(dir.has_value()) << dir.error();
  const std::string path = dir->directory();

  {
    HogDevnodeTmpDir moved(std::move(*dir));
    EXPECT_EQ(moved.directory(), path);
    EXPECT_TRUE(std::filesystem::is_directory(path));
  }
  // [moved] removed the directory on its way out. The moved-from handle is
  // destroyed at the end of the test and must not try to remove it again.
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(HogDevnodeTmpDir, MoveAssignmentRemovesTheOverwrittenDirectory)
{
  std::expected<HogDevnodeTmpDir, std::string> overwritten =
      HogDevnodeTmpDir::create(std::nullopt, "runtime");
  ASSERT_TRUE(overwritten.has_value()) << overwritten.error();
  const std::string overwritten_path = overwritten->directory();

  std::expected<HogDevnodeTmpDir, std::string> kept =
      HogDevnodeTmpDir::create(std::nullopt, "events");
  ASSERT_TRUE(kept.has_value()) << kept.error();
  const std::string kept_path = kept->directory();
  ASSERT_NE(overwritten_path, kept_path);

  *overwritten = std::move(*kept);
  EXPECT_FALSE(std::filesystem::exists(overwritten_path));
  EXPECT_EQ(overwritten->directory(), kept_path);
  EXPECT_TRUE(std::filesystem::is_directory(kept_path));
}

TEST(HogDevnodeDirectories, TemporaryDirectoriesAreDistinctAndRemovedTogether)
{
  std::string paths[4];
  {
    std::expected<HogDevnodeDirectories, std::string> dirs =
        HogDevnodeDirectories::create(std::nullopt);
    ASSERT_TRUE(dirs.has_value()) << dirs.error();
    paths[0] = dirs->runtime.directory();
    paths[1] = dirs->db.directory();
    paths[2] = dirs->event.directory();
    paths[3] = dirs->indexer.directory();
    for (std::string const &path : paths) {
      EXPECT_TRUE(std::filesystem::is_directory(path)) << path;
    }
    EXPECT_EQ(std::set<std::string>(std::begin(paths), std::end(paths)).size(),
              std::size(paths));

    // The node constructor takes the set by value; moving it must not disturb
    // the directories it owns.
    HogDevnodeDirectories moved = std::move(*dirs);
    EXPECT_EQ(moved.event.directory(), paths[2]);
    for (std::string const &path : paths) {
      EXPECT_TRUE(std::filesystem::is_directory(path)) << path;
    }
  }
  for (std::string const &path : paths) {
    EXPECT_FALSE(std::filesystem::exists(path)) << path;
  }
}

TEST(HogDevnodeDirectories, DataDirLayoutIsNamedPerSubsystem)
{
  const std::filesystem::path data_dir = scratch_path("layout");
  std::filesystem::create_directories(data_dir);
  {
    std::expected<HogDevnodeDirectories, std::string> dirs =
        HogDevnodeDirectories::create(data_dir);
    ASSERT_TRUE(dirs.has_value()) << dirs.error();
    EXPECT_EQ(dirs->runtime.directory(),
              std::filesystem::absolute(data_dir / "runtime").string());
    EXPECT_EQ(dirs->db.directory(),
              std::filesystem::absolute(data_dir / "ramdb").string());
    EXPECT_EQ(dirs->event.directory(),
              std::filesystem::absolute(data_dir / "events").string());
    EXPECT_EQ(dirs->indexer.directory(),
              std::filesystem::absolute(data_dir / "indexer").string());
  }
  // A data dir is the operator's; it survives the process that used it.
  EXPECT_TRUE(std::filesystem::is_directory(data_dir / "runtime"));
  EXPECT_TRUE(std::filesystem::is_directory(data_dir / "indexer"));

  std::filesystem::remove_all(data_dir);
}

TEST(HogDevnodeDirectories, PartialFailureNamesTheOffendingPath)
{
  // "events" is the third of the four, so runtime and ramdb are already open
  // when the run fails.
  const std::filesystem::path data_dir = scratch_path("partial");
  std::filesystem::create_directories(data_dir);
  {
    std::ofstream(data_dir / "events") << "not a directory";
  }

  std::expected<HogDevnodeDirectories, std::string> dirs =
      HogDevnodeDirectories::create(data_dir);
  ASSERT_FALSE(dirs.has_value());
  EXPECT_TRUE(dirs.error().contains((data_dir / "events").string()))
      << dirs.error();
  // Under a data dir the directories are the operator's to keep, so they stay
  // put; only the temp-dir flavor is removed on unwind.
  EXPECT_TRUE(std::filesystem::is_directory(data_dir / "runtime"));
  EXPECT_TRUE(std::filesystem::is_directory(data_dir / "ramdb"));

  std::filesystem::remove_all(data_dir);
}

} // namespace
} // namespace hogd
