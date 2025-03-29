#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

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
                    std::shared_ptr<RuntimeContext> runtime_context = nullptr);

  // Disable move and copy.
  SystemCallHandler(SystemCallHandler &&ll) = delete;
  SystemCallHandler &operator=(SystemCallHandler &&ll) = delete;
  SystemCallHandler(const SystemCallHandler &ll) = delete;
  SystemCallHandler &operator=(const SystemCallHandler &ll) = delete;

  // Root invocation of a transaction.  Cannot be called from within a syscall.
  // Starts the invocation that is specified within signed_tx_.
  InvokeResult root_invoke();

  // Create and run a child call frame.
  // Should be called from within a syscall.
  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
  invoke(Invocation const &invocation, GasAmount gas_limit);

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

  std::vector<LogEntry> const &get_logs() const { return committed_logs_; }

  static ContractAddress
  static_get_deploy_address(ContractAddress const &sender,
                            SHA256Hash const &nonce,
                            SHA256Hash const &contract_hash);

  SyscallStatus<ContractAddress>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  bool current_runtime_is_revert_poisoned() const;

  // === Bytestring syscalls ===
  // The returned span is only valid until another storage item is accessed.
  SyscallStatus<std::span<const std::byte>>
  get_bytestring(StorageKey const &storage_key);
  SyscallStatus<void>
  set_bytestring(StorageKey const &storage_key,
                 std::span<const std::byte> input_bytestring);

private:
  std::shared_ptr<RuntimeContext> runtime_context_;
  const SignedTransaction signed_tx_;
  InvocationFrameStack invocation_frame_stack_;

  // Searchable map of contracts that are implicitly created by the current
  // transaction.
  const std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>>
      newly_created_contracts_;

  // only set at the end of the transaction
  std::vector<LogEntry> committed_logs_;

  StorageSnapshotCache &snapshot_cache_;

  // Resolve the WasmiRuntime associated with the queried address.
  // Creates a new runtime if contract is deployed at
  // that address in the current context (i.e. if a contract is deployed there
  // in the snapshot, or by a prior action of the current transaction that was
  // not rolled back by a revert).
  std::unique_ptr<Runtime> resolve_runtime(ContractAddress const &addr);

  // Commit the last call frame, if the last call frame succeeds.
  // Extracts relevant data structures, to prepare for committing the
  // transaction.
  void commit_last_call_frame_success(InvocationFrame &frame);
};

} // namespace hogd
