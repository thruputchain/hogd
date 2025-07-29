#pragma once

#include <memory>
#include <string_view>
#include <unordered_map>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "types.hh"

namespace hogd {

// MultiplexedContext allows multiple RuntimeContexts to coexist, each handling
// different ContractTypes. This enables a single RuntimeContext to delegate
// contract instantiation to specialized contexts based on the contract type.
class MultiplexedContext : public RuntimeContext {
public:
  MultiplexedContext(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>
          contexts);

  std::unique_ptr<Runtime> make_new_runtime(Contract const &contract,
                                            SHA256Hash const &hash,
                                            SystemCallHandler *syscall_handler,
                                            bool do_contract_verify) override;

  bool verify_contract(Contract const &contract) const override;

  ~MultiplexedContext() override = default;

private:
  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs) override;
  std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>> contexts_;
};

} // namespace hogd
