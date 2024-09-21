#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hogd ::test {

[[maybe_unused]]
static std::vector<uint8_t>
load_wasm_from_file(std::string_view filename)
{
  std::string file = std::string(CMAKE_ROOT_BINARY_DIR) +
                     std::string("/hogd/hogexec/runtime/") +
                     std::string(filename);
  FILE *f = std::fopen(file.c_str(), "r");
  if (f == nullptr)
    throw std::runtime_error("failed to load wasm file: " + file);

  std::vector<uint8_t> contents;
  constexpr size_t kBufferSize = 65536;
  char buf[kBufferSize];

  int count = -1;
  while (count != 0) {
    count = std::fread(buf, sizeof(char), kBufferSize, f);
    if (count > 0)
      contents.insert(contents.end(), buf, buf + count);
  }
  std::fclose(f);

  return contents;
}

} // namespace hogd::test
