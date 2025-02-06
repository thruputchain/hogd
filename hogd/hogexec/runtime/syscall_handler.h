#pragma once

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hogd {

class StorageSnapshotCache;

// Data structure that manages all system calls and state associated with a
// running transaction.
// Core components:
//   - Invocation Frame Stack:  A stack of frame objects representing the
//   current state of a running transaction.  Each frame is a different
//   invocation on a contract. A contract might be invoked multiple times and
//   might be reentrant. Each frame either commits or reverts atomically, and
//   pushes its state changes into its parent frame.
//   - Runtime Cache: The set of instantiated WASM runtimes.  One runtime per
//   ContractAddress. This is maintained separately from the frame stack
//   because there is not a 1-1 map between runtimes and call frames.
class SystemCallHandler {
public:
  SystemCallHandler(SignedTransaction signed_tx,
                    StorageSnapshotCache &snapshot_cache,
                    PerTransactionDatabaseProxy &db_proxy,
                    std::optional<NondeterministicProposal> proposal,
                    std::shared_ptr<RuntimeContext> context = nullptr);

  // Disable move and copy.
  SystemCallHandler(SystemCallHandler &&ll) = delete;
  SystemCallHandler &operator=(SystemCallHandler &&ll) = delete;
  SystemCallHandler(const SystemCallHandler &ll) = delete;
  SystemCallHandler &operator=(const SystemCallHandler &ll) = delete;

  // Root invocation of a transaction.  Cannot be called from within a syscall.
  // Starts the invocation that is specified within signed_tx_.
  InvokeResult root_invoke();

  x23::Async<bool> try_commit_transaction();

  // Create and run a child call frame.
  // Should be called from within a syscall.
  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
  invoke(Invocation const &invocation, GasAmount remaining_gas,
         InvocationFlagSet flags);

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
  SyscallStatus<const ReturnBuffer *> get_return_buffer() const;
  // Called by the Return syscall.
  // Sets the buffer to be passed back to the parent call frame.
  // Can be called by root call frame, but in that case will be ignored.
  void set_return_buffer(ReturnBuffer &&returned_buffer);
  bool has_frame_return_been_set() const;

  SyscallStatus<void> log(std::span<const std::byte> message);

  std::vector<LogEntry> const &get_logs() const { return committed_logs_; }

  static ContractAddress
  static_get_deploy_address(ContractAddress const &sender,
                            SHA256Hash const &nonce,
                            SHA256Hash const &contract_hash);

  SyscallStatus<ContractAddress>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  bool current_runtime_is_revert_poisoned() const;

  // TODO(geoff) maybe this logic should move out of SyscallHandler
  SyscallStatus<const Bytestring> get_bytestring(StorageKey const &storage_key);
  SyscallStatus<void>
  set_bytestring(StorageKey const &storage_key,
                 std::span<const std::byte> input_bytestring,
                 StorageDeltaFlags flags);

  SyscallStatus<std::pair<int128_t, int128_t>>
  get_nnint(StorageKey const &storage_key);
  SyscallStatus<void> set_nnint(StorageKey const &storage_key,
                                int128_t set_value, int128_t add_value);

  SyscallStatus<void>
  retrocausal_queue_insert(StorageKey const &key,
                           std::span<const std::byte> queue_entry);
  // if peek is true, return the next entry without advancing the cursor
  HostFnStatus<SyscallStatus<QueueEntry>>
  retrocausal_queue_consume(StorageKey const &key, bool peek);

  SyscallStatus<void> delete_object(StorageKey const &storage_key);

  ProposalModeData const &get_proposal_data() const;

private:
  std::shared_ptr<RuntimeContext> runtime_context_;
  const SignedTransaction signed_tx_;
  InvocationFrameStack invocation_frame_stack_;
  ExecutionModeData execution_mode_data_;

  // Searchable map of contracts that are implicitly created by the current
  // transaction.
  const std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>>
      newly_created_contracts_;

  // only set at the end of the transaction
  std::vector<LogEntry> committed_logs_;
  std::unordered_map<AddressAndKey, FlaggedStorageDelta> committed_deltas_;
  std::unordered_map<ContractAddress, SHA256Hash> committed_deployments_;

  StorageSnapshotCache &snapshot_cache_;
  PerTransactionDatabaseProxy &db_proxy_;

  // Resolve the RuntimeCacheEntry associated with the queried address.
  // Creates a new entry if no entry exists, AND a contract is deployed at
  // that address in the current context (i.e. if a contract is deployed there
  // in the snapshot, or by a prior action of the current transaction that was
  // not rolled back by a revert).
  std::unique_ptr<Runtime> resolve_runtime(ContractAddress const &addr);
  // deployed hash for first arg, true for second arg if it was deployed in the
  // current transaction
  std::pair<std::optional<SHA256Hash>, bool>
  lookup_deployed_contract_hash(ContractAddress const &addr);
  std::shared_ptr<const Contract>
  lookup_contract_by_hash(SHA256Hash const &hash);

  // Commit the last call frame, if the last call frame succeeds.
  // Extracts relevant data structures, to prepare for committing the
  // transaction.
  void commit_last_call_frame_success(InvocationFrame &frame);
  [[nodiscard]] InvokeResult
  finish_nondeterminism_checks(InvokeResult const &res);
};

} // namespace hogd
