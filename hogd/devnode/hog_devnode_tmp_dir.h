#pragma once

#include <string>

namespace hogd {

class HogDevnodeTmpDir {
public:
  HogDevnodeTmpDir();
  ~HogDevnodeTmpDir() noexcept;

  HogDevnodeTmpDir(HogDevnodeTmpDir const &) = delete;
  HogDevnodeTmpDir &operator=(HogDevnodeTmpDir const &) = delete;
  HogDevnodeTmpDir(HogDevnodeTmpDir &&) = delete;
  HogDevnodeTmpDir &operator=(HogDevnodeTmpDir &&) = delete;

  std::string const &directory() const { return path_; }

private:
  std::string path_;
};

} // namespace hogd
