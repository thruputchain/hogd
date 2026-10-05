#pragma once

#include <expected>
#include <memory>

#include "contract.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class SystemCallHandler;

// Gas charged for restoring `image_bytes` of an RV32 contract's declared
// memory image before its first instruction runs. The restore is a byte copy,
// so it is priced at the measured bytestring read-copy rate, rounded up to
// whole blocks. Both constants are already folded into the RV32 gas-schedule
// digest, so metering the restore adds no new committed constant (#4354).
[[nodiscard]] constexpr uint64_t
rv32_memory_restore_gas(uint32_t image_bytes)
{
  const uint64_t blocks =
      (static_cast<uint64_t>(image_bytes) + kBytestringBlockBytes - 1) /
      kBytestringBlockBytes;
  return blocks * kBytestringReadBlockCost;
}

// A translated and ready-to-instantiate RV32 contract.
class Rv32Contract {
public:
  // Translates `contract_blob` under `limits`: the chain's configured bounds
  // at attachment, the VM's ceilings for an already-deployed contract. On
  // failure, names the first artifact check that refused it; code that parses
  // cleanly but does not translate is kInvalidProgram.
  static std::expected<Rv32Contract, rv32::ArtifactError>
  create(ContractBlob const &contract_blob, rv32::Rv32Limits limits);
  Rv32Contract(rv32::ThreadedMachinePreInstantiation pre_instantiation);

  rv32::ThreadedMachine instantiate(uint64_t gas) const;

  // Gas to restore this contract's declared memory image on instantiation.
  [[nodiscard]] uint64_t memory_restore_gas() const
  {
    return rv32_memory_restore_gas(pre_instantiation_.memory_image_size());
  }

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
  InvokeErrorCode
  verify_contract(Contract const &contract, NetworkConfigParams const &params,
                  PreparedContract &out,
                  std::optional<ContractRejectCode> &rejection) const override;
  using RuntimeContext::verify_contract;

  [[nodiscard]] x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) const override;
};

} // namespace hogd
