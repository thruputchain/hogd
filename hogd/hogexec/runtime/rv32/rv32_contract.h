#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include "contract.hh"
#include "hogd/base/xdr_util.h"
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

// One invocation of an RV32 contract. Borrows a const Rv32Contract* owned by
// the context's cache; the context outlives the runtimes it hands out and never
// evicts mid-execution, so the pointer stays valid (no refcounting on the hot
// path).
class Rv32Runtime final : public Runtime {
public:
  Rv32Runtime(Rv32Contract const *contract,
              std::unique_ptr<HostCallContext> host_call_context)
    : Runtime(std::move(host_call_context)), contract_(contract)
  {}

  ~Rv32Runtime() override = default;

private:
  Rv32Contract const *contract_;

  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept override;
};

// RuntimeContext for RV32 contracts. Resolves the deployed contract by hash like
// the wasm context, and memoizes the (expensive) translation -- artifact SHA256
// verification + program decode/fusion -- keyed by contract hash, so it happens
// once per contract rather than once per invocation.
//
// A RuntimeContext is single-threaded, so the cache needs no synchronization,
// and the owned Rv32Contracts have stable addresses (node-based map) handed out
// as const Rv32Contract* to the runtimes. GC is deferred: entries are never
// evicted here. TODO(geoff): move ownership into the block-expiring
// StorageSnapshotCache so translations are reclaimed during a quiescent period.
class Rv32Context final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const &contract) const override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) override;

private:
  std::unordered_map<SHA256Hash, Rv32Contract, ByteArrayHasher>
      translation_cache_;
};

} // namespace hogd
