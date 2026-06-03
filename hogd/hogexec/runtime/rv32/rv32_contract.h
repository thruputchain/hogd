#pragma once

#include "contract.hh"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/threaded_machine.h"

namespace hogd {

class SystemCallHandler;

// A translated and ready-to-instantiate RV32 contract.
class Rv32Contract {
public:
  // TODO(geoff): decide if we want to wire out the several different error
  // categories here.
  static std::optional<Rv32Contract> create(ContractBlob const &contract_blob);
  Rv32Contract(rv32::ThreadedMachinePreInstantiation pre_instantiation);

  ~Rv32Contract();

  rv32::ThreadedMachine instantiate(uint64_t gas) const;

private:
  rv32::ThreadedMachinePreInstantiation pre_instantiation_;
};

class Rv32Runtime final : public Runtime {
public:
  Rv32Runtime(Rv32Contract &contract);

  ~Rv32Runtime() override = default;

private:
  Rv32Contract const &contract_;

  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override;
};

} // namespace hogd
