#pragma once

#include <cstddef>
#include <cstring>
#include <memory>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

namespace hogd {

class SystemCallHandler;
class Runtime;

// Abstract class that wraps the context associated with a sandbox runtime.
// Most sandbox implementations have some amount of shared state (i.e., a list
// of function pointers to link, or an environment allocator).
//
// We assume each RuntimeContext can be used by multiple Runtimes.
// Each RuntimeContext can be used across multiple transactions.
//
// Both entry points are const: resolving a contract and building a Runtime
// must not mutate the context. Any per-invocation state belongs in the
// returned Runtime or behind the SystemCallHandler, never here. An
// implementation that honors that is safe to share across threads, which is
// what lets make_hog_exec_context() hand out one process-wide instance of the
// stateless native/RV32 contexts instead of a per-thread copy whose lifetime
// can end under a longer-lived borrower (issue #2682). Mutating operations
// outside these two (e.g. MockRuntimeContext::set_trampoline) stay non-const
// and are not threadsafe.
class RuntimeContext {
public:
  RuntimeContext() = default;

  // Disable move and copy.
  RuntimeContext(RuntimeContext &&) = delete;
  RuntimeContext &operator=(RuntimeContext &&) = delete;
  RuntimeContext(const RuntimeContext &) = delete;
  RuntimeContext &operator=(const RuntimeContext &) = delete;

  virtual ~RuntimeContext() = default;

  // Perform any required validation checks on the contract.
  // In the case of webassembly, this is the webassembly validator.
  virtual InvokeErrorCode verify_contract(Contract const &contract) const = 0;

  virtual SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) const = 0;
};

// Virtual class that abstracts the interface required for an invocable
// sandbox.  Can be created by an instance of RuntimeContext.
class Runtime {
public:
  // `syscall_handler` is non-owning and must outlive the Runtime. It is the
  // opaque context the sandbox bakes in and hands back to syscall_entry.
  explicit Runtime(SystemCallHandler *syscall_handler);

  using MethodID =
      uint64_t; // from ContractEntryPointID, interpreted in big-endian

  // Disable copy and move
  Runtime(Runtime const &) = delete;
  Runtime &operator=(Runtime const &) = delete;
  Runtime(Runtime &&other) = delete;
  Runtime &operator=(Runtime &&other) = delete;

  InvokeResult invoke(MethodID method_id, uint64_t gas_limit);

  virtual ~Runtime() = default;

protected:
  // Invariant:  The returned GasAmount must be <= the input gas_limit
  virtual std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) = 0;

  SystemCallHandler *syscall_handler_ = nullptr;
};

} // namespace hogd
