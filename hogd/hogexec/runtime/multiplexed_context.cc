#include "hogd/hogexec/runtime/multiplexed_context.h"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

MultiplexedContext::MultiplexedContext(
    std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>> contexts)
  : contexts_(std::move(contexts))
{}

std::unique_ptr<Runtime>
MultiplexedContext::make_new_runtime(Contract const &contract,
                                     SHA256Hash const &hash,
                                     SystemCallHandler *syscall_handler,
                                     bool do_contract_verify)
{
  auto it = contexts_.find(contract.contract_type());
  if (it == contexts_.end()) {
    return nullptr;
  }
  return it->second->make_new_runtime(contract, hash, syscall_handler,
                                      do_contract_verify);
}

bool
MultiplexedContext::verify_contract(Contract const &contract) const
{
  auto it = contexts_.find(contract.contract_type());
  if (it == contexts_.end()) {
    return false;
  }
  return it->second->verify_contract(contract);
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
  return it->second->link_fn_nargs(module_name, fn_name, fn_ptr, nargs);
}
} // namespace hogd
