#pragma once

#include <cstdint>
#include <memory>
#include <optional>

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

  rv32::ThreadedMachine instantiate(uint64_t gas) const;

private:
  rv32::ThreadedMachinePreInstantiation pre_instantiation_;
};

// One invocation of an RV32 contract. Owns its Rv32Contract: the context
// translates the deployed contract fresh per call (no cache yet), so the
// runtime must keep it alive for the machine that borrows its
// pre-instantiation.
class Rv32Runtime final : public Runtime {
public:
  Rv32Runtime(Rv32Contract contract,
              std::unique_ptr<HostCallContext> host_call_context)
    : Runtime(std::move(host_call_context)), contract_(std::move(contract))
  {}

  ~Rv32Runtime() override = default;

private:
  Rv32Contract contract_;

  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept override;
};

// Stateless RuntimeContext for RV32 contracts. Resolves the deployed contract
// the same way the wasm context does -- by hash, on each invocation -- and
// translates its rv32 blob into a fresh Rv32Contract. No caching yet; a future
// version can memoize translations here keyed by contract hash.
class Rv32Context final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const &contract) const override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) override;
};

} // namespace hogd
