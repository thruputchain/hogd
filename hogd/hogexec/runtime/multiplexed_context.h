#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

// MultiplexedContext allows multiple RuntimeContexts to coexist, each handling
// different ContractTypes. This enables a single RuntimeContext to delegate
// contract instantiation to specialized contexts based on the contract type.
class MultiplexedContext : public RuntimeContext {
public:
  MultiplexedContext(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>
          contexts);

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) override;

  InvokeErrorCode verify_contract(Contract const &contract) const override;

  ~MultiplexedContext() override
  {
    for (auto [_, context] : contexts_) {
      if (context.second) {
        delete context.first;
      }
    }
  }

private:
  std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>> contexts_;
};

} // namespace hogd
