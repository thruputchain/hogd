#include "hogd/hogexec/runtime/multiplexed_context.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

MultiplexedContext::MultiplexedContext(
    std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>
        contexts)
  : contexts_(std::move(contexts))
{}

SystemStatus<std::unique_ptr<Runtime>>
MultiplexedContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler)
{
  auto it = contexts_.find(deployed_contract_hash.contract_type);
  if (it == contexts_.end()) {
    return nullptr;
  }
  return it->second.first->make_new_runtime(deployed_contract_hash,
                                            syscall_handler);
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
