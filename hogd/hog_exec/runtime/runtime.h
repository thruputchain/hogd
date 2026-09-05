#pragma once

#include <cstddef>
#include <cstring>
#include <memory>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "x23/async.h"

namespace hogd {

class SystemCallHandler;
class Runtime;
// Defined in prepared_contract.h. Taken by reference below, which is valid
// over an incomplete type, so this widely-included header does not pull in
// the per-runtime machinery that builds a preparation.
struct PreparedContract;

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

  // Perform any required validation checks on the contract, handing back
  // whatever ready-to-instantiate image the check produced.
  //
  // For RV32, verification *is* translation: Rv32Contract::create parses the
  // artifact and builds the threaded pre-instantiation, and nothing cheaper
  // proves translatability (make_new_runtime treats a non-translatable
  // deployed contract as Unrecoverable). Handing that image back lets the
  // caller reuse it instead of lowering the same bytes again (#3536).
  // Runtimes with nothing to prepare leave `out` untouched.
  //
  // `out` is built in place: the caller supplies storage at whatever scope it
  // wants the preparation to live, and the context memoizes nothing. It is
  // written only on success; on rejection it is left as the caller had it.
  virtual InvokeErrorCode verify_contract(Contract const &contract,
                                          PreparedContract &out) const = 0;

  // Convenience for callers that only want the verdict and would discard the
  // preparation. Defined out-of-line so PreparedContract is complete where the
  // discarded instance is constructed and destroyed.
  //
  // Overriders hide this overload; a class whose callers use it through the
  // derived type needs `using RuntimeContext::verify_contract;`.
  InvokeErrorCode verify_contract(Contract const &contract) const;

  virtual x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
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

  x23::Async<InvokeResult> invoke(MethodID method_id, uint64_t gas_limit);

  virtual ~Runtime() = default;

protected:
  // Invariant:  The returned GasAmount must be <= the input gas_limit.
  //
  // MaybeAsync so an implementation that answers without touching the database
  // -- and one that must stay noexcept, which a coroutine cannot (its frame
  // allocation throws before the body runs) -- can return a value directly.
  virtual x23::MaybeAsync<std::pair<InvokeStatus<void>, GasAmount>>
  invoke_internal(MethodID method_id, uint64_t gas_limit) = 0;

  SystemCallHandler *syscall_handler_ = nullptr;
};

} // namespace hogd
