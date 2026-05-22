#include "hogd/hogd_dev/hogd_dev_tmp_dir.h"

#include <filesystem>
#include <system_error>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"

namespace hogd {

HogdDevTmpDir::HogdDevTmpDir()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(dir.has_value());
  path_ = std::filesystem::absolute((*dir).get_path()).string();
}

// NOLINTNEXTLINE(bugprone-exception-escape)
HogdDevTmpDir::~HogdDevTmpDir() noexcept
{
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  if (ec) {
    warn("could not remove hogd-dev temp dir '{}': error {}", path_,
         ec.value());
  }
}

} // namespace hogd
