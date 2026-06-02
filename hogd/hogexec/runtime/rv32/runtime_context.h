#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/dispatch.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

// Test-runtime image after artifact parsing/lowering. Production activation
// still needs a deployed-artifact store and selector; this is not that layer.
struct RuntimeContractImage {
  std::shared_ptr<const Program> program;
  std::vector<uint8_t> initial_memory;
  // Test-built images default to the historical single method-0 entry. Parsed
  // F2 artifacts always provide their explicit dispatch table.
  std::vector<DispatchEntry> dispatch_table = {
      DispatchEntry{.method_id = 0, .pc = 0}};
  uint32_t memory_bytes = kMaxMemoryBytes;
  HostFn8 entry = nullptr;
};

// Experimental test-only RuntimeContext for the RV32 VM. It deliberately uses
// kMockTest contracts so RV32 remains outside protocol/runtime selection.
class RuntimeContext final : public hogd::RuntimeContext {
public:
  explicit RuntimeContext(
      std::unordered_map<int32_t, RuntimeContractImage> contracts);

  InvokeErrorCode verify_contract(Contract const &contract) const override;

  SystemStatus<std::unique_ptr<hogd::Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) override;

private:
  std::unordered_map<int32_t, RuntimeContractImage> contracts_;
};

} // namespace hogd::rv32
