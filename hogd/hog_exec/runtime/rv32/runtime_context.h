#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/program.h"

namespace hogd::rv32 {

// Test-runtime image after artifact parsing/lowering. Production activation
// still needs a deployed-artifact store and selector; this is not that layer.
//
// v4 enters every call at PC 0 (_start) with the invoked method id seeded into
// a0/a1; the guest dispatches. There is no host-side method -> PC table.
struct RuntimeContractImage {
  std::shared_ptr<const Program> program;
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kMaxMemoryBytes;
  SyscallFunction entry = nullptr;
};

// Experimental test-only RuntimeContext for the RV32 VM. It deliberately uses
// kMockTest contracts so RV32 remains outside protocol/runtime selection.
class RuntimeContext final : public hogd::RuntimeContext {
public:
  explicit RuntimeContext(
      std::unordered_map<int32_t, RuntimeContractImage> contracts);

  InvokeErrorCode verify_contract(Contract const &contract) const override;

  SystemStatus<std::unique_ptr<hogd::Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override;

private:
  std::unordered_map<int32_t, RuntimeContractImage> contracts_;
};

} // namespace hogd::rv32
