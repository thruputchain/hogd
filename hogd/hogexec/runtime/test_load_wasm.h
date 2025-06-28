#pragma once

#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "contract.hh"

namespace hogd ::test {

static ContractBlob
load_raw_file(std::string_view filename)
{
  std::string file = std::string(CMAKE_ROOT_BINARY_DIR) + std::string(filename);
  FILE *f = std::fopen(file.c_str(), "r");
  if (f == nullptr)
    throw std::runtime_error("failed to load wasm file: " + file);

  ContractBlob contents;
  constexpr size_t kBufferSize = 65536;
  char buf[kBufferSize];

  int count = -1;
  while (count != 0) {
    count = std::fread(buf, sizeof(char), kBufferSize, f);
    std::span<char> buf_span(&buf[0], count);
    if (count > 0)
      contents.append_range(buf_span);
  }
  std::fclose(f);

  return contents;
}

[[maybe_unused]]
static ContractBlob
load_wasm_from_file(std::string_view filename)
{
  return load_raw_file("/hogd/hogexec/runtime/" + std::string(filename));
}

[[maybe_unused]]
static std::shared_ptr<Contract>
load_test_contract(std::string_view filename)
{
  auto wasm = load_raw_file("/test_contracts/" +
                            std::string(filename));
  auto out = std::make_shared<Contract>(ContractType::kWasmi);
  out->wasmi_contract() = std::move(wasm);
  return out;
}

} // namespace hogd::test
