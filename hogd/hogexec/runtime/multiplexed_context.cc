#include "hogd/hogexec/runtime/multiplexed_context.h"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "account.hh"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

MultiplexedContext::MultiplexedContext(
    std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>
        contexts)
  : contexts_(std::move(contexts))
{}

SystemStatus<std::unique_ptr<Runtime>>
MultiplexedContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler, bool do_contract_verify)
{
  auto it = contexts_.find(deployed_contract_hash.contract_type);
  if (it == contexts_.end()) {
    return nullptr;
  }
  return it->second.first->make_new_runtime(deployed_contract_hash,
                                            syscall_handler,
                                            do_contract_verify);
}

bool
MultiplexedContext::verify_contract(Contract const &contract) const
{
  auto it = contexts_.find(contract.contract_type());
  if (it == contexts_.end()) {
    return false;
  }
  return it->second.first->verify_contract(contract);
}

bool
MultiplexedContext::link_fn_nargs(std::string_view module_name,
                                  std::string_view fn_name, void *fn_ptr,
                                  uint8_t nargs)
{
  auto it = contexts_.find(ContractType::kWasmi);
  if (it == contexts_.end()) {
    return false;
  }
  return it->second.first->link_fn_nargs(module_name, fn_name, fn_ptr, nargs);
}
} // namespace hogd
