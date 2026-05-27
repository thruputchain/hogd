#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

struct RuntimeContractImage {
  std::shared_ptr<const Program> program;
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kMaxMemoryBytes;
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
