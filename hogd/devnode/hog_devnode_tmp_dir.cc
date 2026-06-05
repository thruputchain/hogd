#include "hogd/devnode/hog_devnode_tmp_dir.h"

#include <filesystem>
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

// NOLINTNEXTLINE(bugprone-exception-escape)
HogDevnodeTmpDir::~HogDevnodeTmpDir() noexcept
{
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  if (ec) {
    warn("could not remove hog-devnode temp dir '{}': error {}", path_,
         ec.value());
  }
}

} // namespace hogd
