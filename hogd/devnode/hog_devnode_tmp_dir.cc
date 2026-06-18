#include "hogd/devnode/hog_devnode_tmp_dir.h"

#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <system_error>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"

namespace hogd {

HogDevnodeTmpDir::HogDevnodeTmpDir()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(dir.has_value());
  path_ = std::filesystem::absolute((*dir).get_path()).string();
}

HogDevnodeTmpDir::HogDevnodeTmpDir(
    std::optional<std::filesystem::path> const &data_dir,
    std::string_view subdir)
{
  if (!data_dir.has_value()) {
    ErrnoStatus<Directory> dir = Directory::open_temporary();
    CHECK(dir.has_value());
    path_ = std::filesystem::absolute((*dir).get_path()).string();
    return;
  }

  std::filesystem::path path = *data_dir / std::filesystem::path(subdir);
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    throw std::runtime_error(
        std::format("could not create hog-devnode data directory '{}': {}",
                    path.string(), ec.message()));
  }
  if (!std::filesystem::is_directory(path, ec)) {
    if (ec) {
      throw std::runtime_error(
          std::format("could not inspect hog-devnode data directory '{}': {}",
                      path.string(), ec.message()));
    }
    throw std::runtime_error(
        std::format("hog-devnode data path '{}' is not a directory",
                    path.string()));
  }

  path_ = std::filesystem::absolute(path).string();
  remove_on_destroy_ = false;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
HogDevnodeTmpDir::~HogDevnodeTmpDir() noexcept
{
  if (!remove_on_destroy_) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  if (ec) {
    warn("could not remove hog-devnode temp dir '{}': {}", path_, ec.message());
  }
}

} // namespace hogd
