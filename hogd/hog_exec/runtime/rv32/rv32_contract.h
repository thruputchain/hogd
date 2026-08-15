#pragma once

#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "types.hh"

namespace hogd {

class SystemCallHandler;

// Gas charged per byte-block of the initial memory image copied when a
// ThreadedMachine is instantiated: one gas unit per this many bytes (floored).
// Instantiating a contract copies its full, zero-padded initial memory image
// (up to rv32::kMaxMemoryBytes) into a fresh per-invocation slot before any
// guest instruction runs, so this setup work scales with the artifact's memory
// size and must be metered -- otherwise a caller can force repeated large host
// copies for free by invoking with a zero (or below-cost) child gas limit (see
// issue #3408). A byte-block rate keeps the charge negligible for the small
// images typical contracts use while bounding the copy volume an attacker can
// force per unit of gas. This is a host-side runtime charge applied at invoke
// time, not baked into artifacts, so it is intentionally independent of the
// artifact-committed rv32::kGasScheduleDigest.
inline constexpr uint64_t kRv32InstantiationBytesPerGas = 256;

// A translated and ready-to-instantiate RV32 contract.
class Rv32Contract {
public:
  // TODO(geoff): decide if we want to wire out the several different error
  // categories here.
  static std::optional<Rv32Contract> create(ContractBlob const &contract_blob);
  Rv32Contract(rv32::ThreadedMachinePreInstantiation pre_instantiation);

  rv32::ThreadedMachine instantiate(uint64_t gas) const;

  // Gas cost of copying this contract's initial memory image during
  // instantiate(). Charged before a machine is built (see Rv32Runtime::
  // invoke_internal).
  [[nodiscard]] uint64_t instantiation_gas_cost() const noexcept
  {
    return pre_instantiation_.initial_memory_image_size() /
           kRv32InstantiationBytesPerGas;
  }

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
  Rv32Contract const *contract_;

  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept override;
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
