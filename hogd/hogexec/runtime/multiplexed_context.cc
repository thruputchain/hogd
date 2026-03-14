#include "hogd/hogexec/runtime/multiplexed_context.h"

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

size_t
MultiplexedContext::context_index(ContractType type)
{
  switch (type) {
  case ContractType::kWasmi:
    return 0;
  case ContractType::kNative:
    return 1;
  case ContractType::kMockTest:
    return 2;
  }
  throw std::invalid_argument("unsupported contract type");
}

MultiplexedContext::MultiplexedContext(
    std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>
        contexts)
{
  for (auto &[type, context] : contexts) {
    contexts_[context_index(type)] = {.context = context.first,
                                      .owned = context.second};
  }
}

MultiplexedContext::MultiplexedContext(RuntimeContextEntry wasmi_context,
                                       RuntimeContextEntry native_context)
  : MultiplexedContext(wasmi_context, native_context, {})
{}

MultiplexedContext::MultiplexedContext(RuntimeContextEntry wasmi_context,
                                       RuntimeContextEntry native_context,
                                       RuntimeContextEntry mock_context)
  : contexts_({wasmi_context, native_context, mock_context})
{}

SystemStatus<std::unique_ptr<Runtime>>
MultiplexedContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler, bool do_contract_verify)
{
  auto &entry = contexts_[context_index(deployed_contract_hash.contract_type)];
  if (entry.context == nullptr) {
    return nullptr;
  }
  return entry.context->make_new_runtime(deployed_contract_hash,
                                         syscall_handler, do_contract_verify);
}

bool
MultiplexedContext::verify_contract(Contract const &contract) const
{
  auto &entry = contexts_[context_index(contract.contract_type())];
  if (entry.context == nullptr) {
    return false;
  }
  return entry.context->verify_contract(contract);
}
} // namespace hogd
