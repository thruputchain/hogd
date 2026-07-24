#pragma once

#include <memory>
#include <optional>

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

// One invocation of an RV32 contract. Borrows a const Rv32Contract* that
// outlives it: either owned by the block-scoped StorageSnapshotCache (snapshot
// deployments) or by the per-transaction SystemCallHandler's translation cache
// (transaction-local deployments). Both are node-stable and outlive the
// runtimes they hand out, so the pointer stays valid with no refcounting.
class Rv32Runtime final : public Runtime {
public:
  Rv32Runtime(Rv32Contract const *contract, SystemCallHandler *syscall_handler)
    : Runtime(syscall_handler), contract_(contract)
  {}

  ~Rv32Runtime() override = default;

private:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept override;

  Rv32Contract const *contract_;
};

// RuntimeContext for RV32 contracts. Snapshot deployments arrive with their
// translation already built in the StorageSnapshotCache, reached via
// DeployedContract's back-pointer; transaction-local deployments are translated
// into (and borrowed from) the SystemCallHandler's per-transaction translation
// cache. The context itself holds no per-contract state.
class Rv32Context final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const &contract) const override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override;
};

} // namespace hogd
