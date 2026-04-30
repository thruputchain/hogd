#pragma once

#include <cassert>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_token.hh"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_meta_concept.h"
#include "hogd/hogexec/runtime/transaction_stats.h"
#include "invoke_flag.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/vec.h"

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
//   shared across runtime instances and across transactions.
//   Not implemented to be threadsafe.
class SystemCallHandler {
public:
  SystemCallHandler(Transaction const &tx, StorageSnapshotCache &snapshot_cache,
                    PerTransactionDatabaseProxy db_proxy,
                    PerTransactionEventServerProxy event_server_proxy,
                    ExecutionModeCtor execution_mode_ctor,
                    RuntimeContext &runtime_context);

  // Constructor for test harness, mock infrastructure
  SystemCallHandler(StorageSnapshotCache &snapshot_cache,
                    PerTransactionDatabaseProxy db_proxy,
                    PerTransactionEventServerProxy event_server_proxy,
                    ExecutionModeCtor execution_mode_ctor,
                    RuntimeContext &runtime_context, AuthClaimData auth_claims,
                    SHA256Hash source_tx_hash);

  // Disable move and copy.
  SystemCallHandler(SystemCallHandler &&ll) = delete;
  SystemCallHandler &operator=(SystemCallHandler &&ll) = delete;
  SystemCallHandler(const SystemCallHandler &ll) = delete;
  SystemCallHandler &operator=(const SystemCallHandler &ll) = delete;

  ~SystemCallHandler()
  {
    if constexpr (is_debug_build()) {
      if (!transaction_committed_or_aborted_) {
        warn("SystemCallHandler destroyed without committing or aborting "
             "transaction");
        if (debug_force_commit_abort_) {
          panic("SystemCallHandler destroyed at invalid state: {}",
                transaction_committed_or_aborted_);
        }
      }
      if (last_frame_.has_value() &&
          !last_frame_.value().extract_events().empty() &&
          root_invoke_succeeded_ && !two_phase_commit_failed_) {
        panic("SystemCallHandler destroyed with events to emit");
      }
    }
  }

  // Debug management operations
  void enable_transaction_stats()
  {
    stats_ = std::make_unique<TransactionStats>();
  }

  auto *get_transaction_stats(this auto &self) { return self.stats_.get(); }

  // Root invocation of a transaction.  Cannot be called from within a syscall.
  // Must only be called once per transaction.
  InvokeResult root_invoke();

  // Lower level functions to manipulate the invocation stack.
  // These should ONLY be used within the testing framework
  // and from the SDK FFI bindings
  //
  // Returns the parent trace, if any, which should be stashed and restored in
  // finish_frame
  std::unique_ptr<InvocationFrameTrace>
  start_frame(Invocation invocation, InvokeFlagSet flags,
              std::span<uint32_t const> owned_auth_token_ids) noexcept;
  SystemStatus<std::pair<InvokeResult, bool>> finish_frame(
      InvokeStatus<void> result,
      std::unique_ptr<InvocationFrameTrace> stashed_parent_trace) noexcept;

  InvocationFrameOutput get_most_recent_frame_output() const;

  void enable_tracing();
  InvocationFrameTrace const *get_trace() const;

  void start_syscall_trace(SyscallNo syscall_number,
                           GasAmount current_gas_remaining);
  void finish_syscall_trace(HostFnStatus<SyscallErrorCode> status,
                            GasAmount gas_remaining_at_end);

  bool last_frame_finished() const noexcept { return last_frame_finished_; }
  bool is_running() const noexcept { return !invocation_frame_stack_.empty(); }

  // Copy events from the currently active invocation frame.
  x23::Vec<TxEmittedEvent> copy_active_frame_events() const
  {
    CHECK(!invocation_frame_stack_.empty());
    return invocation_frame_stack_.copy_active_frame_events();
  }

  // Copy events from the last (root) frame after it has finished.
  x23::Vec<TxEmittedEvent> copy_last_frame_events() const
  {
    CHECK(last_frame_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return last_frame_.value().get_events();
  }

  // Called after the root invocation frame finishes (by root_invoke or
  // the mock path).  Runs final validation on cumulative_invoke_result_
  // and sets root_invoke_succeeded_ if appropriate.
  void finalize_after_root_frame();

  // Allow the mock path to set cumulative_invoke_result_ before calling
  // finalize_after_root_frame().  Must be called before finalization.
  void mock_set_cumulative_invoke_result(InvokeResult result)
  {
    assert(!finalized_);
    cumulative_invoke_result_ = result;
  }

  template<SyscallNo Syscall> auto *get_syscall_trace_data() noexcept
  {
    if constexpr (Syscall == kNoTracingSyscall) {
      return static_cast<void *>(nullptr);
    }
    else {
      using return_t =
          std::remove_cvref_t<decltype(get<std::to_underlying(Syscall) + 1>(
              invocation_frame_trace_->system_calls.back().body))>;

      if (!invocation_frame_trace_) {
        return static_cast<return_t *>(nullptr);
      }
      assert(!invocation_frame_trace_->system_calls.empty());
      try {
        return &get<std::to_underlying(Syscall) + 1>(
            invocation_frame_trace_->system_calls.back().body);
      } catch (const std::exception &e) {
        panic("Failed to get syscall trace data for {}: {}", Syscall, e.what());
      }
    }
  }
  // Operations after root_invoke()

  // Attempt to commit the current transaction to the database.
  // Requirement: root_invoke() must be called first.
  // Calls abort_transaction if either the commit fails or
  // the root_invoke earlier had an error.
  ExecResult try_commit_transaction();
  SystemStatus<void> abort_transaction();
  // Emit the transaction's event log to the event server.
  // Called for all deterministic outcomes (OK, DeterministicError, Reverted).
  x23::Async<SystemStatus<void>> emit_events(ExecResult exec_result);

  // Get the proposal data.
  // Can only be called after root_invoke()
  // (as otherwise the proposal data is not available)
  NondeterministicProposalData const &get_proposal_data() const;

  // Syscalls: operations within an invocation

  // Create and run a child call frame.
  // Should be called from within a syscall.
  [[nodiscard]] std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
  invoke(ContractAddress const &address, Runtime::MethodID method,
         GasAmount gas_limit, Calldata calldata, InvokeFlagSet flags,
         std::span<uint32_t const> owned_auth_token_ids);

  // syscall methods: can only be called when call_frame_stack_ is nonempty
  // (i.e. while at least one invoke() is running)
  const ContractAddress &get_current_address() const;

  SyscallStatus<ContractAddress> get_msg_sender() const;
  SHA256Hash get_source_tx_hash() const;

  // return the calldata associated with the current active invocation.
  // Incorrect to call method when no invocation is running.
  // Reference invalidated when call frame changes.
  Calldata const &get_calldata() const;

  // Get the auth tokens supplied by the parent frame.
  // Incorrect to call method when no invocation is running.
  // Span invalidated when call frame changes.
  std::span<const uint32_t> get_parent_supplied_auth_tokens() const;

  AuthClaimData const &get_auth_claims() const;

  // Get the buffer returned by a child call frame.
  // Error if most recent child did call set_return_buffer.
  SystemStatus<SyscallStatus<const ReturnBuffer *>> get_return_buffer() const;
  std::span<const uint32_t> get_returned_auth_token_ids() const;
  bool check_auth_tokens_owned(std::span<uint32_t const> token_ids) const;
  // Called by the Return syscall.
  // Sets the buffer to be passed back to the parent call frame.
  // Can be called by root call frame, but in that case will be ignored.
  void set_return_buffer(ReturnBuffer &&returned_buffer, bool revert_frame,
                         std::vector<uint32_t> returned_auth_token_ids = {});

  SyscallStatus<void> log_event(Topic const *topic0, Topic const *topic1,
                                Topic const *topic2,
                                std::span<const std::byte> data);
  SyscallStatus<void> log_event(uint32_t num_topics,
                                std::span<const std::byte> data);

  static ContractAddress
  static_get_deploy_address(ContractAddress const &sender,
                            SHA256Hash const &nonce,
                            SHA256Hash const &contract_hash);

  SystemStatus<SyscallStatus<ContractAddress>>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  bool is_read_only() const
  {
    return invocation_frame_stack_.is_active_frame_read_only();
  }

  // === Crypto syscalls ===
  SyscallStatus<void> sha256_update(uint32_t input_index,
                                    std::span<const std::byte> input_buffer,
                                    bool input_reset);
  SyscallStatus<SHA256Hash> sha256_finalize(uint32_t input_index);

  // === Bytestring syscalls ===
  // The returned span is only valid until another storage item is accessed.
  SystemStatus<SyscallStatus<const Bytestring *>>
  get_bytestring(StorageKeyContractData const &storage_key);
  SystemStatus<SyscallStatus<void>>
  set_bytestring(StorageKeyContractData const &storage_key,
                 std::span<const std::byte> input_bytestring,
                 StorageDeltaFlagSet input_flags);

  // === Nonnegative Integer syscalls ===
  // get_nnint returns the base value and the add value.
  // Returns the base value from the snapshot if load_from_snapshot is true.
  // Otherwise, returns a tx-local written set value, if one exists,
  // or nullopt if not.
  SystemStatus<SyscallStatus<std::pair<std::optional<int128_t>, int128_t>>>
  get_nnint(StorageKeyContractData const &storage_key, bool load_from_snapshot);

  SystemStatus<SyscallStatus<void>>
  set_nnint(StorageKeyContractData const &storage_key,
            std::optional<int128_t> set_value, const int128_t &add_value,
            StorageDeltaFlagSet input_flags);

  // === Delete syscall ===
  SystemStatus<SyscallStatus<void>>
  delete_object(StorageKeyContractData const &storage_key,
                StorageDeltaFlagSet flags);

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

  SystemStatus<SyscallStatus<void>>
  set_address_nonce_config(NonceConfig const &nonce_config);

  SystemStatus<SyscallStatus<void>> set_address_gas_permission_configs(
      x23::Vec<GasPermissionConfig, kMaxGasConfigs> gas_permission_configs);

  // Auth token syscalls
  SyscallStatus<uint32_t> mint_auth_token(AuthToken const &auth_token);
  SyscallStatus<InvocationFrameStack::ConsumeAuthTokenResult>
  consume_auth_token(uint32_t token_id, BlockHeight current_height);

  // == Block statistics syscalls ==
  // These are based on reading the db tip.
  SystemStatus<BlockHeight> get_block_height() const;

  // == General access methods ==
  //
  // returns nullptr if the contract does not exist
  SystemStatus<const Contract *> resolve_contract(SHA256Hash const &hash) const;

  // == Error converstion methods ==
  static ExecResult to_exec_result(InvokeErrorCode error_code);
  static ExecResult to_exec_result(SystemErrorCode error_code);

  static auto force_commit_or_abort()
  {
    debug_force_commit_abort_ = true;
    return x23::Defer([]() { debug_force_commit_abort_ = false; });
  }

  // TODO(geoff): remove this once tests are updated to not use reentrance
  void hack_dont_use_test_bypass_allow_reentrance()
  {
    hack_DONTUSE_TEST_BYPASS_allow_reentrance_ = true;
  }

private:
  RuntimeContext &runtime_context_;
  PerTransactionDatabaseProxy db_proxy_;
  PerTransactionEventServerProxy event_server_proxy_;
  InvocationFrameStack invocation_frame_stack_;
  std::unique_ptr<InvocationFrameTrace> invocation_frame_trace_;

  const SHA256Hash source_tx_hash_;
  const AuthClaimData auth_claims_;
  std::optional<Invocation> root_invocation_;

  // Searchable map of contracts that are implicitly created by the current
  // transaction.
  const std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>,
                           ByteArrayHasher>
      newly_created_contracts_;

  ExecutionModeData execution_mode_data_;

  std::unique_ptr<TransactionStats> stats_ = nullptr;

  // only set at the end of the transaction
  std::optional<InvocationFrame> last_frame_;
  InvokeResult cumulative_invoke_result_ =
      InvokeResult{.result_ = {}, .gas_consumed_ = 0};
  bool root_invoke_succeeded_{false};
  bool finalized_{false};
  bool last_frame_finished_{false};
  bool transaction_committed_or_aborted_{false};
  bool two_phase_commit_failed_{false};

  StorageSnapshotCache &snapshot_cache_;

  inline static bool debug_force_commit_abort_{false};

  bool hack_DONTUSE_TEST_BYPASS_allow_reentrance_{false};

  // Validate that the transaction can be run safely.
  // Ensures, for example, that the wasms are valid.
  // Sets cumulative_invoke_result_ to the appropriate error if validation
  // fails.
  void perform_initial_validation();

  // check the validation data at the end of a transaction, if in validation
  // mode
  void check_final_validation_data();

  // Convert the HostFnStatus returned from the root invocation frame into an
  // InvokeStatus for the top level return.
  static InvokeStatus<void>
  handle_root_host_fn_status(HostFnStatus<SyscallErrorCode> status);

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
};

} // namespace hogd
