#pragma once

#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/wasm/instrumented_bytes.h"

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
      contents.data.append_range(buf_span);
  }
  std::fclose(f);

  // Test WATs may import multiple host functions; use a permissive limit.
  constexpr uint32_t kTestMaxImportedFunctions = 16;
  auto instrumented = InstrumentedBytes::instrument(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(
                                   contents.data.data()),
                               contents.data.size()),
      kTestMaxImportedFunctions);
  if (!instrumented) {
    throw std::runtime_error("failed to instrument wasm file: " + file);
  }
  contents.data.clear();
  contents.data.append_range(instrumented->data());

  return contents;
}

[[maybe_unused]]
static ContractBlob
load_wasm_from_file(std::string_view filename)
{
  return load_raw_file("/hogd/hogexec/runtime/wasm/" + std::string(filename));
}

} // namespace hogd::test
