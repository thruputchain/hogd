#include "contract_loader.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "contract.hh"
#include "x23/x23/progname.h"

namespace hogd {

namespace {

constexpr std::string_view kSystemContractsDir =
    "/usr/share/groundhog/contracts";

std::mutex contract_artifact_directories_mutex;
std::vector<std::filesystem::path> contract_artifact_directories;

} // namespace

void
set_contract_artifact_directories(std::vector<std::filesystem::path> directories)
{
  std::lock_guard guard(contract_artifact_directories_mutex);
  contract_artifact_directories = std::move(directories);
}

std::vector<std::filesystem::path>
contract_search_directories()
{
  std::lock_guard guard(contract_artifact_directories_mutex);
  std::vector<std::filesystem::path> directories = contract_artifact_directories;
  directories.push_back(std::filesystem::path(CMAKE_ROOT_BINARY_DIR) /
                        "built_contracts");
  directories.push_back(std::filesystem::path(x23::getprogpath())
                            .parent_path() /
                        "contracts");
  directories.emplace_back(kSystemContractsDir);
  return directories;
}

namespace {

ContractBlob
load_raw_file(std::string_view filename)
{
  std::vector<std::filesystem::path> search_directories =
      contract_search_directories();
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

  std::size_t count = 1;
  while (count != 0) {
    count = std::fread(buf, sizeof(char), kBufferSize, f);
    std::span<char> buf_span(&buf[0], count);
    if (count > 0)
      contents.data.append_range(buf_span);
  }
  const bool read_error = std::ferror(f) != 0;
  std::fclose(f);
  if (read_error) {
    throw std::runtime_error("failed to read wasm file: " +
                             std::string(filename));
  }

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
