#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hogd :: test {


[[maybe_unused]]
static std::vector<uint8_t>
load_wasm_from_file(const char *filename)
{
  std::string file(filename);
  FILE *f = std::fopen(file.c_str(), "r");

  if (f == nullptr) {
    throw std::runtime_error("failed to load wasm file: " + file);
  }

  std::vector<uint8_t> contents;

  const int k_buf_size = 65536;
  char buf[k_buf_size];

  int count = -1;
  while (count != 0) {
    count = std::fread(buf, sizeof(char), k_buf_size, f);
    if (count > 0) {
      contents.insert(contents.end(), buf, buf + count);
    }
  }
  std::fclose(f);

  return contents;
}

} // namespace hogd
