#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hogd {

class HogDevnodeTmpDir {
public:
  HogDevnodeTmpDir();
  HogDevnodeTmpDir(std::optional<std::filesystem::path> const &data_dir,
                   std::string_view subdir);
  ~HogDevnodeTmpDir() noexcept;

  HogDevnodeTmpDir(HogDevnodeTmpDir const &) = delete;
  HogDevnodeTmpDir &operator=(HogDevnodeTmpDir const &) = delete;
  HogDevnodeTmpDir(HogDevnodeTmpDir &&) = delete;
  HogDevnodeTmpDir &operator=(HogDevnodeTmpDir &&) = delete;

  std::string const &directory() const { return path_; }

private:
  std::string path_;
  bool remove_on_destroy_ = true;
};

} // namespace hogd
