#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "contract.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "x23/async.h"

namespace hogd::rv32 {

// Canonical test-machine image. Entry is PC 0 with the method ID in a0/a1;
// the guest dispatches. Production preparation is owned by Rv32Contract.
struct RuntimeContractImage {
  std::shared_ptr<const Program> program;
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kDefaultMaxMemoryBytes;
  SyscallFunction entry = nullptr;
};

// Canonical test runtime using kMockTest contracts. Production RV32 contracts
// use Rv32Context in rv32_contract.h.
class RuntimeContext final : public hogd::RuntimeContext {
public:
  explicit RuntimeContext(
      std::unordered_map<int32_t, RuntimeContractImage> contracts);

  InvokeErrorCode
  verify_contract(Contract const &contract, NetworkConfigParams const &params,
                  hogd::PreparedContract &out,
                  std::optional<ContractRejectCode> &rejection) const override;
  using hogd::RuntimeContext::verify_contract;

  [[nodiscard]] x23::Async<SystemStatus<std::unique_ptr<hogd::Runtime>>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) const override;

private:
  std::unordered_map<int32_t, RuntimeContractImage> contracts_;
};

} // namespace hogd::rv32
