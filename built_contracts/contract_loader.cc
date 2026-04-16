#include "contract_loader.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "hogd/hogexec/runtime/wasm/instrumented_bytes.h"
#include "x23/x23/progname.h"

namespace hogd {

namespace {

ContractBlob
load_raw_file(std::string_view filename)
{
  std::vector<std::filesystem::path> search_directories{
      std::filesystem::path(CMAKE_ROOT_BINARY_DIR) / "built_contracts",
      std::filesystem::path(x23::getprogpath()).remove_filename()};
  FILE *f = nullptr;
  for (const std::filesystem::path &search_dir : search_directories) {
    std::filesystem::path path = search_dir / filename;
    f = std::fopen(path.c_str(), "r");
    if (f) {
      break;
    }
  }
  if (!f) {
    throw std::runtime_error("failed to load wasm file: " +
                             std::string(filename));
  }

  ContractBlob contents;
  constexpr size_t kBufferSize = 65536;
  char buf[kBufferSize];

  int count = -1;
  while (count != 0) {
    count = std::fread(buf, sizeof(char), kBufferSize, f);
    std::span<char> buf_span(&buf[0], count);
    if (count > 0)
      contents.wasm.append_range(buf_span);
  }
  std::fclose(f);

  // groundhog build produces already-instrumented wasm; no need to re-instrument.
  return contents;
}

} // namespace

ContractBlob
load_contract_blob(std::string_view filename)
{
  return load_raw_file(filename);
}

std::shared_ptr<Contract>
load_contract(std::string_view filename)
{
  auto wasm = load_contract_blob(filename);
  auto out = std::make_shared<Contract>(ContractType::kWasmi);
  out->wasmi_contract() = std::move(wasm);
  return out;
}

} // namespace hogd
