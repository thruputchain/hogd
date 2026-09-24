#pragma once

#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class SystemCallHandler;

// A translated and ready-to-instantiate RV32 contract.
class Rv32Contract {
public:
  // TODO(geoff): decide if we want to wire out the several different error
  // categories here.
  // Translates `contract_blob` under `limits`: the chain's configured bounds
  // at attachment, the VM's ceilings for an already-deployed contract.
  static std::optional<Rv32Contract> create(ContractBlob const &contract_blob,
                                            rv32::Rv32Limits limits);
  Rv32Contract(rv32::ThreadedMachinePreInstantiation pre_instantiation);

  rv32::ThreadedMachine instantiate(uint64_t gas) const;

private:
  rv32::ThreadedMachinePreInstantiation pre_instantiation_;
};

// Production invocation. Borrows its prepared contract from
// StorageSnapshotCache or the transaction's SystemCallHandler translation
// cache. Both keep entries at stable addresses and outlive the runtimes
// borrowing them.
class Rv32Runtime final : public Runtime {
public:
  Rv32Runtime(Rv32Contract const *contract, SystemCallHandler *syscall_handler)
    : Runtime(syscall_handler), contract_(contract)
  {}

  ~Rv32Runtime() override = default;

private:
  Rv32Contract const *contract_;

  // This noexcept wrapper is not a coroutine: creating run_ecall_loop() can
  // throw before its body starts. The wrapper catches frame-allocation failure
  // and returns Unrecoverable.
  x23::MaybeAsync<std::pair<InvokeStatus<void>, GasAmount>>
  invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept override;

  x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
  run_ecall_loop(rv32::ThreadedMachine machine, uint64_t gas_limit);
};

// RuntimeContext for RV32 contracts. Snapshot deployments borrow preparation
// through DeployedContract's cache back-pointer. Transaction-local deployments
// resolve preparation from attachment validation or the snapshot cache. The
// context itself holds no per-contract state.
class Rv32Context final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const &contract,
                                  NetworkConfigParams const &params,
                                  PreparedContract &out) const override;
  using RuntimeContext::verify_contract;

  [[nodiscard]] x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) const override;
};

} // namespace hogd
