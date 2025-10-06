#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>

#include "account.hh"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/transaction_stats.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class StorageSnapshotCache;

// TODO(geoff): root_invoke() validates any incoming wasm blobs.
// We may later do that validation preemptively in the coordinator.
// We may also wish to add checks to make sure that a wasm that's created
// is actually deployed, and/or put limits on the size of an individual wasm.
// What's done here is only check well-formedness of the wasm.

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
                    PerTransactionEventServerProxy event_server_proxy,
                    ExecutionModeCtor execution_mode_ctor,
                    std::shared_ptr<RuntimeContext> runtime_context);

  // Disable move and copy.
  SystemCallHandler(SystemCallHandler &&ll) = delete;
  SystemCallHandler &operator=(SystemCallHandler &&ll) = delete;
  SystemCallHandler(const SystemCallHandler &ll) = delete;
  SystemCallHandler &operator=(const SystemCallHandler &ll) = delete;

  // Debug management operations
  void enable_transaction_stats()
  {
    stats_ = std::make_unique<TransactionStats>();
  }

  auto *get_transaction_stats(this auto &self) { return self.stats_.get(); }

  // Root invocation of a transaction.  Cannot be called from within a syscall.
  // Starts the invocation that is specified within signed_tx_.
  // Must only be called once per transaction.
  InvokeResult root_invoke();

  // Operations after root_invoke()

  // Attempt to commit the current transaction to the database.
  // Requirement: root_invoke() must be called first.
  // Returns true if the transaction commits, false otherwise.
  // Calls abort_transaction if either the commit fails or
  // the root_invoke earlier had an error.
  SystemStatus<bool> try_commit_transaction();

  // Get the proposal data.
  // Can only be called after root_invoke()
  // (as otherwise the proposal data is not available)
  NondeterministicProposalData const &get_proposal_data() const;

  // Syscalls: operations within an invocation

  // Create and run a child call frame.
  // Should be called from within a syscall.
  [[nodiscard]] std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
  invoke(ContractAddress const &address, MethodID method, GasAmount gas_limit,
         Calldata calldata, InvokeFlagSet flags);

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
  void set_return_buffer(ReturnBuffer &&returned_buffer, bool revert_frame);

  void log_event(SHA256Hash const *topic0, SHA256Hash const *topic1,
                 SHA256Hash const *topic2, std::span<const std::byte> data);

  static ContractAddress
  static_get_deploy_address(ContractAddress const &sender,
                            SHA256Hash const &nonce,
                            SHA256Hash const &contract_hash);

  SystemStatus<SyscallStatus<ContractAddress>>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  bool is_read_only() const
  {
    assert(!invocation_frame_stack_.empty());
    return invocation_frame_stack_.get_active_frame().is_read_only();
  }

  // === Bytestring syscalls ===
  // The returned span is only valid until another storage item is accessed.
  SystemStatus<SyscallStatus<std::span<const std::byte>>>
  get_bytestring(StorageKeyContractData const &storage_key);
  SystemStatus<SyscallStatus<void>>
  set_bytestring(StorageKeyContractData const &storage_key,
                 std::span<const std::byte> input_bytestring);

  // === Nonnegative Integer syscalls ===
  // get_nnint returns the base value and the add value.
  // Returns the base value from the snapshot if load_from_snapshot is true.
  // Otherwise, returns a tx-local written set value, if one exists,
  // or nullopt if not.
  SystemStatus<SyscallStatus<std::pair<std::optional<int128_t>, int128_t>>>
  get_nnint(StorageKeyContractData const &storage_key, bool load_from_snapshot);

  SystemStatus<SyscallStatus<void>>
  set_nnint(StorageKeyContractData const &storage_key,
            std::optional<int128_t> set_value, int128_t add_value);

  // === Delete syscall ===
  SystemStatus<SyscallStatus<void>>
  delete_object(StorageKeyContractData const &storage_key,
                StorageItemRPCFlagSet flags);

  // === Retrocausal Queue syscalls ===
  SystemStatus<SyscallStatus<void>>
  retrocausal_queue_insert(StorageKeyContractData const &key,
                           std::span<const std::byte> queue_entry);
  SystemStatus<SyscallStatus<QueueEntry>>
  retrocausal_queue_consume(StorageKeyContractData const &key, bool peek);

  // == Native token syscalls ==
  SystemStatus<SyscallStatus<int128_t>>
  get_native_token_balance(ContractAddress const &contract_address);
  SystemStatus<SyscallStatus<void>>
  modify_native_token_balance(ContractAddress const &contract_address,
                              int128_t transfer_amount);

  // == Authentication syscalls ==
  SystemStatus<SyscallStatus<void>>
  change_public_key_registration(uint32_t key_index,
                                 PublicKeyConfigOpt new_key);

private:
  std::shared_ptr<RuntimeContext> runtime_context_;
  const SignedTransaction signed_tx_;
  PerTransactionDatabaseProxy db_proxy_;
  PerTransactionEventServerProxy event_server_proxy_;
  InvocationFrameStack invocation_frame_stack_;

  // Searchable map of contracts that are implicitly created by the current
  // transaction.
  const std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>>
      newly_created_contracts_;

  ExecutionModeData execution_mode_data_;

  std::unique_ptr<TransactionStats> stats_ = nullptr;

  // only set at the end of the transaction
  std::optional<InvocationFrame> last_frame_;
  InvokeResult cumulative_invoke_result_ =
      InvokeResult{.result_ = {}, .gas_consumed_ = 0};
  bool root_invoke_succeeded_{false};
  bool transaction_committed_or_aborted_{false};

  StorageSnapshotCache &snapshot_cache_;

  // Validate that the transaction can be run safely.
  // Ensures, for example, that the wasms are valid.
  // Sets cumulative_invoke_result_ to the appropriate error if validation
  // fails.
  void perform_initial_validation();

  // check the validation data at the end of a transaction, if in validation
  // mode
  void check_final_validation_data();

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

  BlockExecutionMode get_block_execution_mode() const
  {
    return std::holds_alternative<ProposalModeData>(execution_mode_data_)
               ? BlockExecutionMode::kBlockExecutionModeProposal
               : BlockExecutionMode::kBlockExecutionModeValidation;
  }

  // Get the hash of the proposed transaction.
  // Can only be called after root_invoke()
  // (as otherwise the proposal data is not available)
  SHA256Hash get_proposed_tx_hash() const;

  SystemStatus<void> emit_events();
};

} // namespace hogd
