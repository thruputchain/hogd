#pragma once

#include <string>

namespace hogd {

class HogdDevTmpDir {
public:
  HogdDevTmpDir();
  ~HogdDevTmpDir() noexcept;

  HogdDevTmpDir(HogdDevTmpDir const &) = delete;
  HogdDevTmpDir &operator=(HogdDevTmpDir const &) = delete;
  HogdDevTmpDir(HogdDevTmpDir &&) = delete;
  HogdDevTmpDir &operator=(HogdDevTmpDir &&) = delete;

  std::string const &directory() const { return path_; }

private:
  std::string path_;
};

} // namespace hogd
