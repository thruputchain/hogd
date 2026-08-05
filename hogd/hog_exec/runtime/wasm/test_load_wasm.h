#pragma once

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/build_config.h"
#include "hogd/hog_exec/runtime/wasm/instrumented_bytes.h"

namespace hogd ::test {

// Returns std::nullopt if the file cannot be read or fails instrumentation.
// Instrumentation legitimately rejects some inputs (e.g. modules with a start
// section), and tests assert on that, so this must report rather than abort.
static std::optional<ContractBlob>
try_load_raw_file(std::string_view filename)
{
  std::string file = std::string(kBuildRoot) + std::string(filename);
  FILE *f = std::fopen(file.c_str(), "r");
  if (f == nullptr)
    return std::nullopt;

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
    return std::nullopt;
  }
  contents.data.clear();
  contents.data.append_range(instrumented->data());

  return contents;
}

[[maybe_unused]]
static std::optional<ContractBlob>
try_load_wasm_from_file(std::string_view filename)
{
  return try_load_raw_file("/hogd/hog_exec/runtime/wasm/" +
                           std::string(filename));
}

// Aborts if the file cannot be loaded. Tests that expect a load to fail must
// use try_load_wasm_from_file and assert on the empty result.
[[maybe_unused]]
static ContractBlob
load_raw_file(std::string_view filename)
{
  auto contents = try_load_raw_file(filename);
  if (!contents) {
    panic("failed to load wasm file: {}{}", kBuildRoot, filename);
  }
  return std::move(contents).value();
}

[[maybe_unused]]
static ContractBlob
load_wasm_from_file(std::string_view filename)
{
  return load_raw_file("/hogd/hog_exec/runtime/wasm/" + std::string(filename));
}

} // namespace hogd::test
