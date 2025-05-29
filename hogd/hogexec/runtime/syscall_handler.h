#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

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
//   - Storage Snapshot Cache reference: Object in which lookups to the database
//   are cached (reads).
//   - Wasmi Context: Stores any relevant state internal to Wasmi.  Can be
//   shared across runtime instances (and across transactions, if made
//   threadsafe by appropriate guards).
class SystemCallHandler {
public:
  SystemCallHandler(SignedTransaction signed_tx,
                    StorageSnapshotCache &snapshot_cache,
                    PerTransactionDatabaseProxy db_proxy,
                    ExecutionModeData execution_mode_data,
                    std::shared_ptr<RuntimeContext> runtime_context = nullptr);

  // Disable move and copy.
  SystemCallHandler(SystemCallHandler &&ll) = delete;
  SystemCallHandler &operator=(SystemCallHandler &&ll) = delete;
  SystemCallHandler(const SystemCallHandler &ll) = delete;
  SystemCallHandler &operator=(const SystemCallHandler &ll) = delete;

  // Root invocation of a transaction.  Cannot be called from within a syscall.
  // Starts the invocation that is specified within signed_tx_.
  InvokeResult root_invoke();

  // Attempt to commit the current transaction to the database.
  // Requirement: root_invoke() must be called first, and
  // that must have returned successfully.
  x23::Async<SystemStatus<bool>> try_commit_transaction();

  // Create and run a child call frame.
  // Should be called from within a syscall.
  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
  invoke(Invocation const &invocation, GasAmount gas_limit,
         InvokeFlagSet flags);

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

  SyscallStatus<void> log(std::span<const std::byte> message);

  static ContractAddress
  static_get_deploy_address(ContractAddress const &sender,
                            SHA256Hash const &nonce,
                            SHA256Hash const &contract_hash);

  SystemStatus<SyscallStatus<ContractAddress>>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  bool current_runtime_is_revert_poisoned() const;

  // === Bytestring syscalls ===
  // The returned span is only valid until another storage item is accessed.
  SystemStatus<SyscallStatus<std::span<const std::byte>>>
  get_bytestring(StorageKeyContractData const &storage_key);
  SystemStatus<SyscallStatus<void>>
  set_bytestring(StorageKeyContractData const &storage_key,
                 std::span<const std::byte> input_bytestring);

  // === Nonnegative Integer syscalls ===
  SystemStatus<SyscallStatus<std::pair<int128_t, int128_t>>>
  get_nnint(StorageKeyContractData const &storage_key);
  SystemStatus<SyscallStatus<void>>
  set_nnint(StorageKeyContractData const &storage_key, int128_t set_value,
            int128_t add_value);

  // === Delete syscall ===
  SystemStatus<SyscallStatus<void>>
  delete_object(StorageKeyContractData const &storage_key);

  // === Retrocausal Queue syscalls ===
  SystemStatus<SyscallStatus<void>>
  retrocausal_queue_insert(StorageKeyContractData const &key,
                           std::span<const std::byte> queue_entry);
  SystemStatus<SyscallStatus<QueueEntry>>
  retrocausal_queue_consume(StorageKeyContractData const &key, bool peek);

  // == Native token syscalls ==
  SystemStatus<int128_t>
  get_native_token_balance(ContractAddress const &contract_address);
  SystemStatus<SyscallStatus<void>>
  modify_native_token_balance(ContractAddress const &contract_address,
                              int128_t transfer_amount);

private:
  std::shared_ptr<RuntimeContext> runtime_context_;
  const SignedTransaction signed_tx_;
  PerTransactionDatabaseProxy db_proxy_;
  InvocationFrameStack invocation_frame_stack_;

  // Searchable map of contracts that are implicitly created by the current
  // transaction.
  const std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>>
      newly_created_contracts_;

  ExecutionModeData execution_mode_data_;

  // only set at the end of the transaction
  std::optional<InvocationFrame> last_frame_;
  bool root_invoked_succeeded_ = false;
  bool transaction_committed_ = false;

  StorageSnapshotCache &snapshot_cache_;

  // Resolve the WasmiRuntime associated with the queried address.
  // Creates a new runtime if contract is deployed at
  // that address in the current context (i.e. if a contract is deployed there
  // in the snapshot, or by a prior action of the current transaction that was
  // not rolled back by a revert).
  SystemStatus<std::unique_ptr<Runtime>>
  resolve_runtime(ContractAddress const &addr);

  // Commit the last call frame, if the last call frame succeeds.
  // Extracts relevant data structures, to prepare for committing the
  // transaction.
  void commit_last_call_frame_success(InvocationFrame &&frame);
};

} // namespace hogd
