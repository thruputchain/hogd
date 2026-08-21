#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "types.hh"

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
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override;

  InvokeErrorCode verify_contract(Contract const &contract) const override;

  InvokeErrorCode
  verify_and_cache_contract(Contract const &contract, SHA256Hash const &hash,
                            SystemCallHandler &handler) const override;

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
