#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "contract.hh"
#include "hogd/hogexec/runtime/item/deployed_contract.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/threaded_machine.h"
#include "types.hh"

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

// One invocation of an RV32 contract. Either borrows a const Rv32Contract*
// owned by the snapshot cache (stable address; snapshot deployments), or owns
// a freshly translated Rv32Contract (transaction-local deployments). Runtime is
// non-movable, so the interior pointer in the owning case stays valid.
class Rv32Runtime final : public Runtime {
public:
  Rv32Runtime(Rv32Contract const *contract, SystemCallHandler *syscall_handler)
    : Runtime(syscall_handler), contract_(contract)
  {}

  Rv32Runtime(Rv32Contract contract, SystemCallHandler *syscall_handler)
    : Runtime(syscall_handler), owned_contract_(std::move(contract)),
      contract_(&owned_contract_.value())
  {}

  ~Rv32Runtime() override = default;

private:
  std::optional<Rv32Contract> owned_contract_;
  Rv32Contract const *contract_;

  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept override;
};

// RuntimeContext for RV32 contracts. Resolves the deployed contract by hash
// like the wasm context. Snapshot deployments arrive with their translation
// already built (in the StorageSnapshotCache, reached via DeployedContract's
// back-pointer); transaction-local deployments are translated on the fly and
// owned by the Rv32Runtime. The context itself holds no per-contract state.
class Rv32Context final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const &contract) const override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override;
};

} // namespace hogd
