#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "x23/async.h"

namespace hogd {

// MultiplexedContext allows multiple RuntimeContexts to coexist, each handling
// different ContractTypes. This enables a single RuntimeContext to delegate
// contract instantiation to specialized contexts based on the contract type.
class MultiplexedContext : public RuntimeContext {
public:
  MultiplexedContext(
      std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>
          contexts);

  [[nodiscard]] x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) const override;

  InvokeErrorCode
  verify_contract(Contract const &contract, NetworkConfigParams const &params,
                  PreparedContract &out,
                  std::optional<ContractRejectCode> &rejection) const override;
  using RuntimeContext::verify_contract;

  ~MultiplexedContext() override
  {
    for (auto [_, context] : contexts_) {
      if (context.second) {
        delete context.first;
      }
    }
  }

private:
  std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>
      contexts_;
};

} // namespace hogd
