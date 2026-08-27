#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"

namespace hogd {

MultiplexedContext::MultiplexedContext(
    std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>
        contexts)
  : contexts_(std::move(contexts))
{}

SystemStatus<std::unique_ptr<Runtime>>
MultiplexedContext::make_new_runtime(DeployedContract const &deployed_contract,
                                     SystemCallHandler *syscall_handler) const
{
  auto it = contexts_.find(deployed_contract.hash.contract_type);
  if (it == contexts_.end()) {
    // verify_contract rejects unregistered types before deployment, so a
    // deployed contract with one is an internal inconsistency, not a
    // contract fault.
    error("Unrecoverable: no runtime context registered for contract type {}",
          deployed_contract.hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  return it->second.first->make_new_runtime(deployed_contract, syscall_handler);
}

InvokeErrorCode
MultiplexedContext::verify_contract(Contract const &contract) const
{
  auto it = contexts_.find(contract.contract_type());
  if (it == contexts_.end()) {
    return InvokeErrorCode::Deterministic;
  }
  return it->second.first->verify_contract(contract);
}
} // namespace hogd
