#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include "hogd/base/log.h"

namespace hogd {

class TmpDir {
public:
  TmpDir() : directory_(create_tmp_directory()) {}
  ~TmpDir() { CHECK_GT(std::filesystem::remove_all(directory_), 0UL); }

  std::string directory() const { return directory_; }

private:
  static std::string create_tmp_directory()
  {
    std::string dir;
    do {
      dir = "/tmp/" + time_string_with_ns();
    } while (!std::filesystem::create_directory(dir));
    return dir;
  }

  static std::string time_string_with_ns()
  {
    std::chrono::time_point<std::chrono::system_clock> now =
        std::chrono::system_clock::now();
    auto sec = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - sec).count();
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S") << '.'
        << std::setw(9) << std::setfill('0') << ns;
    return oss.str();
  }

  const std::filesystem::path directory_;
};

} // namespace hogd
