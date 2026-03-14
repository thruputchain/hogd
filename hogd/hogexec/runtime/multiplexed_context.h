#pragma once

#include <array>
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
  struct RuntimeContextEntry {
    RuntimeContext *context = nullptr;
    bool owned = false;
  };

  MultiplexedContext(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>
          contexts);
  MultiplexedContext(RuntimeContextEntry wasmi_context,
                     RuntimeContextEntry native_context);
  MultiplexedContext(RuntimeContextEntry wasmi_context,
                     RuntimeContextEntry native_context,
                     RuntimeContextEntry mock_context);

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler,
                   bool do_contract_verify) override;

  bool verify_contract(Contract const &contract) const override;

  ~MultiplexedContext() override
  {
    for (auto context : contexts_) {
      if (context.owned) {
        delete context.context;
      }
    }
  }

private:
  static size_t context_index(ContractType type);

  std::array<RuntimeContextEntry, 3> contexts_{};
};

} // namespace hogd
