#pragma once

#include <cstdint>
#include <map>

#include "hogd/hogexec/runtime/call_frame.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class StorageSnapshotCache;

// Data structure that manages all system calls and state associated with a
// running transaction.
// Core components:
//   - Call Frame Stack:  A stack of CallFrame objects representing the current
//   state of a running transaction.  Each CallFrame is a different invocation
//   on a contract. A contract might be invoked multiple times and might be
//   reentrant. Each CallFrame either commits or reverts atomically, and pushes
//   its state changes into its parent CallFrame.
//   - Runtime Cache: The set of instantiated WASM runtimes.  One runtime per
//   ContractAddress. This is maintained separately from the CallFrame stack
//   because there is not a 1-1 map between runtimes and call frames.
class SystemCallHandler {
public:
  SystemCallHandler(SignedTransaction signed_tx,
                    StorageSnapshotCache &snapshot_cache);

  // Root invocation of a transaction.  Cannot be called from within a syscall.
  // Starts the invocation that is specified within signed_tx_.
  InvokeResult root_invoke();

  // Create and run a child call frame.
  // Should be called from within a syscall.
  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
  invoke(Invocation const &invocation);

  // syscall methods: can only be called when call_frame_stack_ is nonempty
  // (i.e. while at least one invoke() is running)
  ContractAddress get_current_address() const;

  SyscallStatus<ContractAddress> get_msg_sender() const;
  SHA256Hash get_source_tx_hash() const;

  // return the calldata associated with the current active invocation.
  // Incorrect to call method when no invocation is running.
  // Reference invalidated when call frame changes.
  Calldata const &get_calldata() const;

  // Get the buffer returned by a child call frame.
  // Error if most recent child did call set_return_buffer.
  SyscallStatus<const ReturnBuffer> get_return_buffer() const;
  // Called by the Return syscall.
  // Sets the buffer to be passed back to the parent call frame.
  // Can be called by root call frame, but in that case will be ignored.
  void set_return_buffer(ReturnBuffer &&returned_buffer);

  SyscallStatus<void> log(std::span<const std::byte> message);

  std::vector<LogEntry> const &get_logs() const { return committed_logs_; }

  static ContractAddress
  static_get_deploy_address(ContractAddress const &sender,
                            SHA256Hash const &nonce,
                            SHA256Hash const &contract_hash);

  SyscallStatus<ContractAddress>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  bool current_runtime_is_revert_poisoned() const;

private:
  struct RuntimeCacheEntry {
    WasmiRuntime runtime_;
    const bool local_deployment_;

    // When a call frame reverts, it reverts all of the state changes
    // made in that frame (and all child frames).
    // We do not revert the internal details of individual WasmiRuntimes
    // (we could, in theory; but this is expensive and would require
    // very careful management of Wasmi's internal data structures).
    // Instead, we mark any contract that was reverted as `revert_poisoned`,
    // and prevent any further invocations on that runtime.
    // Future PRs will allow metadata to allow a contract to mark itself
    // as `revert_safe`.
    bool revert_poisoned_ = false;

    RuntimeCacheEntry(WasmiRuntime &&runtime, bool local_deployment)
      : runtime_(std::move(runtime)), local_deployment_(local_deployment)
    {}
    RuntimeCacheEntry(RuntimeCacheEntry &) = delete;
  };

  WasmiContext wasmi_context_;

  const SignedTransaction signed_tx_;

  std::vector<CallFrame> call_frame_stack_;

  // RuntimeCacheEntry cannot be moved while it is actively invoked.  unique_ptr
  // required here.
  std::map<ContractAddress, std::unique_ptr<RuntimeCacheEntry>> runtimes_;

  // Searchable map of contracts that are implicitly created by the current
  // transaction.
  const std::map<SHA256Hash, std::shared_ptr<const Contract>>
      newly_created_contracts_;

  // only set at the end of the transaction
  std::vector<LogEntry> committed_logs_;

  StorageSnapshotCache &snapshot_cache_;

  // Resolve the RuntimeCacheEntry associated with the queried address.
  // Creates a new entry if no entry exists, AND a contract is deployed at
  // that address in the current context (i.e. if a contract is deployed there
  // in the snapshot, or by a prior action of the current transaction that was
  // not rolled back by a revert).
  RuntimeCacheEntry *resolve_runtime(ContractAddress const &addr);

  // Get the amount of gas available to a child invoke call.
  // UINT64_MAX as gas_limit implies gas_limit is whatever gas is left.
  // Otherwise, checks the current runtime's available gas,
  // and returns an error if it has insufficient gas to supply
  // the requested amount.
  SyscallStatus<GasAmount>
  get_invoke_gas_limit(GasAmount call_specified_gas_limit);

  void start_call_frame(Invocation const &invocation);
  void finish_call_frame(InvokeResult const &result);

  // Lookup the hash of the contract deployed at the queried address,
  // _only_ for contracts that are deployed during the current transaction.
  // Allows deployments to be created and then invoked during one transaction
  // (useful for initialization).
  //
  // Also usable to check whether a deployment was rolled back by a reverted
  // call frame. (even if a corresponding runtime entry exists).
  std::optional<SHA256Hash>
  lookup_locally_deployed_contract(ContractAddress const &addr) const;

  auto &get_active_call_frame(this auto &self)
  {
    assert(!self.call_frame_stack_.empty());
    return self.call_frame_stack_.back();
  }

  // Commit the last call frame, if the last call frame succeeds.
  // Extracts relevant data structures, to prepare for committing the
  // transaction.
  void commit_last_call_frame_success(CallFrame &frame);
};

} // namespace hogd
