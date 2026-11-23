#include "hogd/devnode/hog_devnode_tmp_dir.h"

#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"

namespace hogd {

namespace {

// Subsystem working directories, as named under an operator's data dir.
constexpr char kRuntimeSubdir[] = "runtime";
constexpr char kRamDbSubdir[] = "ramdb";
constexpr char kEventSubdir[] = "events";
constexpr char kIndexerSubdir[] = "indexer";

// open_temporary_dir creates a fresh temp dir under $TMPDIR.
std::expected<std::string, std::string>
open_temporary_dir()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  if (!dir) {
    return std::unexpected(
        std::format("could not create a hog-devnode temp dir: {} (set TMPDIR "
                    "to a writable directory)",
                    dir.error()));
  }
  return std::filesystem::absolute((*dir).get_path()).string();
}

// open_data_subdir creates [data_dir]/[subdir] and restricts it to its owner.
std::expected<std::string, std::string>
open_data_subdir(std::filesystem::path const &data_dir, std::string_view subdir)
{
  std::filesystem::path path = data_dir / std::filesystem::path(subdir);
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return std::unexpected(
        std::format("could not create hog-devnode data directory '{}': {}",
                    path.string(), ec.message()));
  }
  if (!std::filesystem::is_directory(path, ec)) {
    if (ec) {
      return std::unexpected(
          std::format("could not inspect hog-devnode data directory '{}': {}",
                      path.string(), ec.message()));
    }
    return std::unexpected(
        std::format("hog-devnode data path '{}' is not a directory",
                    path.string()));
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  if (ec) {
    return std::unexpected(
        std::format("could not secure hog-devnode data directory '{}': {}",
                    path.string(), ec.message()));
  }
  return std::filesystem::absolute(path).string();
}

} // namespace

std::expected<HogDevnodeTmpDir, std::string>
HogDevnodeTmpDir::create(std::optional<std::filesystem::path> const &data_dir,
                         std::string_view subdir)
{
  if (!data_dir.has_value()) {
    std::expected<std::string, std::string> path = open_temporary_dir();
    if (!path) {
      return std::unexpected(std::move(path.error()));
    }
    return HogDevnodeTmpDir(std::move(*path), /*remove_on_destroy=*/true);
  }

  std::expected<std::string, std::string> path =
      open_data_subdir(*data_dir, subdir);
  if (!path) {
    return std::unexpected(std::move(path.error()));
  }
  return HogDevnodeTmpDir(std::move(*path), /*remove_on_destroy=*/false);
}

HogDevnodeTmpDir::HogDevnodeTmpDir(HogDevnodeTmpDir &&other) noexcept
  : path_(std::move(other.path_)),
    remove_on_destroy_(std::exchange(other.remove_on_destroy_, false))
{}

HogDevnodeTmpDir &
HogDevnodeTmpDir::operator=(HogDevnodeTmpDir &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  release();
  path_ = std::move(other.path_);
  remove_on_destroy_ = std::exchange(other.remove_on_destroy_, false);
  return *this;
}

HogDevnodeTmpDir::~HogDevnodeTmpDir() noexcept
{
  release();
}

void
HogDevnodeTmpDir::release() noexcept
{
  if (!std::exchange(remove_on_destroy_, false)) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  if (ec) {
    warn("could not remove hog-devnode temp dir '{}': {}", path_, ec.message());
  }
}

std::expected<HogDevnodeDirectories, std::string>
HogDevnodeDirectories::create(
    std::optional<std::filesystem::path> const &data_dir)
{
  std::expected<HogDevnodeTmpDir, std::string> runtime =
      HogDevnodeTmpDir::create(data_dir, kRuntimeSubdir);
  if (!runtime) {
    return std::unexpected(std::move(runtime.error()));
  }
  std::expected<HogDevnodeTmpDir, std::string> db =
      HogDevnodeTmpDir::create(data_dir, kRamDbSubdir);
  if (!db) {
    return std::unexpected(std::move(db.error()));
  }
  std::expected<HogDevnodeTmpDir, std::string> event =
      HogDevnodeTmpDir::create(data_dir, kEventSubdir);
  if (!event) {
    return std::unexpected(std::move(event.error()));
  }
  std::expected<HogDevnodeTmpDir, std::string> indexer =
      HogDevnodeTmpDir::create(data_dir, kIndexerSubdir);
  if (!indexer) {
    return std::unexpected(std::move(indexer.error()));
  }
  return HogDevnodeDirectories{.runtime = std::move(*runtime),
                               .db = std::move(*db),
                               .event = std::move(*event),
                               .indexer = std::move(*indexer)};
}

} // namespace hogd
