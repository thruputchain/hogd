#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/wasm/wasm_test_framework.h"
#include "hogd/hog_exec/runtime/wasm/wasmtime_runtime.h"
#include "hogd/init.h"
#include "hogd/rust/wasmtime/bindings.h"
#include "types.hh"

namespace hogd {

namespace {

// Load a WASM file from disk. The contracts in built_contracts/ are already
// instrumented at build time by groundhog-cli (they carry the
// __groundhog_stack_limit export and embedded spec sections), so we measure the
// deployed artifact as-is — the runtime verifies but does not re-instrument it.
std::vector<uint8_t>
load_wasm_file(std::string const &filename)
{
  FILE *f = std::fopen(filename.c_str(), "rb");
  if (f == nullptr) {
    panic("Failed to open file: {}", filename);
  }

  std::vector<uint8_t> contents;
  constexpr size_t kBufferSize = 65536;
  char buf[kBufferSize];

  int count = -1;
  while (count != 0) {
    count = std::fread(buf, sizeof(char), kBufferSize, f);
    if (count > 0) {
      contents.insert(contents.end(), buf, buf + count);
    }
  }
  std::fclose(f);

  return contents;
}

// Process a single contract file and print its stats
void
process_contract(std::string const &contract_name, std::string const &filename)
{
  // Load WASM file
  auto wasm_data = load_wasm_file(filename);
  size_t contract_size = wasm_data.size();

  // Create contract object
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract().data.append_range(wasm_data);

  // Compute hash
  SHA256Hash hash = sha256(contract);
  DeployedContractHash deployed_hash{.hash = hash,
                                     .contract_size =
                                         static_cast<uint32_t>(contract_size),
                                     .contract_type = ContractType::kWasmi};

  WasmTestFramework framework;
  framework.set_context(make_hog_exec_context());
  framework.make_syscall_handler(contract);
  CHECK(framework.get_syscall_handler() != nullptr);

  // Create runtime
  auto runtime_result = framework.get_context()->make_new_runtime(
      deployed_hash, framework.get_syscall_handler());

  if (!runtime_result.has_value()) {
    std::println(stderr, "Failed to create runtime for {}", contract_name);
    return;
  }

  auto *wasmtime_runtime =
      dynamic_cast<WasmtimeRuntime *>(runtime_result.value().get());
  if (wasmtime_runtime == nullptr) {
    std::println(stderr, "Runtime is not a WasmtimeRuntime for {}",
                 contract_name);
    return;
  }

  // Get stats
  WasmtimeModuleStats stats{};
  bool success = wasmtime_runtime->get_module_stats(&stats);

  if (success) {
    std::println(
        "contract={} contract_size={} serialized_module_size={} ratio={}",
        contract_name, contract_size, stats.serialized_module_size,
        static_cast<double>(stats.serialized_module_size) / contract_size);
  }
  else {
    std::println(stderr, "Failed to get stats for {}", contract_name);
  }
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (argc < 2) {
    std::println(stderr, "Usage: {} <wasm_file1> [wasm_file2] ...", argv[0]);
    return 1;
  }

  if (hogd::hogd_init(argc, argv, false) != 0) {
    return 1;
  }

  // Process each contract file
  for (int i = 1; i < argc; i++) {
    std::string filename = argv[i];
    std::filesystem::path path(filename);
    std::string contract_name = path.filename().string();

    try {
      hogd::process_contract(contract_name, filename);
    } catch (std::exception const &e) {
      std::println(stderr, "Error processing {}: {}", contract_name, e.what());
      exit(EXIT_FAILURE);
    }
  }

  return 0;
}
