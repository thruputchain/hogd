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
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_token.hh"
#include "contract.hh"
#include "db_conflict.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/invocation_frame.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_meta_concept.h"
#include "hogd/hog_exec/runtime/transaction_stats.h"
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
                    RuntimeContext &runtime_context, AuthClaim auth_claim,
                    SHA256Hash source_tx_hash);

  // Disable move and copy.
  SystemCallHandler(SystemCallHandler &&ll) = delete;
  SystemCallHandler &operator=(SystemCallHandler &&ll) = delete;
  SystemCallHandler(const SystemCallHandler &ll) = delete;
  SystemCallHandler &operator=(const SystemCallHandler &ll) = delete;

  ~SystemCallHandler();

  // Debug management operations
  void enable_transaction_stats()
  {
    stats_ = std::make_unique<TransactionStats>();
  }

  auto *get_transaction_stats(this auto &self) { return self.stats_.get(); }

  // Root invocation of a transaction.  Cannot be called from within a syscall.
  // Must only be called once per transaction.
  x23::Async<InvokeResult> root_invoke();

  // Lower level functions to manipulate the invocation stack.
  // These should ONLY be used within the testing framework
  // and from the SDK FFI bindings
  //
  // On success returns the parent trace, if any, which should be stashed and
  // restored in finish_frame. Returns an Unrecoverable SystemError if child
  // frame admission fails under memory pressure; no child frame is pushed in
  // that case, so finish_frame must not be called (#3058).
  //
  // max_sha256_buffers is the frame's hash-buffer limit, read from the tip by
  // the caller: that read may suspend, and this is a noexcept boundary the
  // test FFI mock path enters directly, so it cannot be a coroutine.
  [[nodiscard]] SystemStatus<std::unique_ptr<InvocationFrameTrace>>
  start_frame(Invocation invocation, InvokeFlagSet flags,
              std::span<uint32_t const> owned_auth_token_ids,
              uint32_t max_sha256_buffers) noexcept;
  x23::MaybeAsync<SystemStatus<std::pair<InvokeResult, bool>>> finish_frame(
      InvokeStatus<void> result,
      std::unique_ptr<InvocationFrameTrace> stashed_parent_trace) noexcept;

  // Body of finish_frame(). Not noexcept: it is the coroutine, and its frame
  // allocation is what finish_frame() catches on its behalf.
  x23::Async<SystemStatus<std::pair<InvokeResult, bool>>>
  finish_frame_impl(InvokeStatus<void> result,
                    std::unique_ptr<InvocationFrameTrace> stashed_parent_trace);

  InvocationFrameOutput get_most_recent_frame_output() const;
  x23::Vec<TxEmittedEvent> get_most_recent_frame_events() const;

  void enable_tracing();
  InvocationFrameTrace const *get_trace() const;

  void start_syscall_trace(SyscallNo syscall_number,
                           GasAmount current_gas_remaining);
  void finish_syscall_trace(HostFnStatus<SyscallErrorCode> status,
                            GasAmount gas_remaining_at_end);

  bool last_frame_finished() const noexcept { return last_frame_finished_; }
  bool is_running() const noexcept { return !invocation_frame_stack_.empty(); }

  // Pin the block's db tip on this handler, if it is not pinned already.
  // Idempotent, and called from every path that can reach a tip-derived
  // value: perform_initial_validation() before the transaction body runs, and
  // validate_new_invocation()/start_frame(), which together are the only ways
  // a frame is admitted (the view and test-FFI paths enter at one or the
  // other rather than through root_invoke()). The fetch itself can fail as a
  // system error; every later read is infallible.
  //
  // Only the block's first fetch reaches the database and suspends; once the
  // cache holds the tip -- and on every call after this handler pinned it --
  // this answers from MaybeAsync's value arm without allocating a frame.
  [[nodiscard]] x23::MaybeAsync<SystemStatus<void>> init_db_tip() const;

  // Admission checks a new invocation frame must pass, in the order their
  // error codes take precedence, grouped by the question each answers:
  //
  //   can this frame make any such call -- valid flags, no reentrance, room
  //   under the cross-invoke depth bound;
  //   does this call hold up -- auth tokens owned by the caller.
  //
  // Reentrance and the depth bound are conditions of the calling frame, so
  // they outrank the request's own validity: a reentrant or too-deep invoke
  // reports that even when it also forwards a token it does not own (#2973
  // established this for reentrance; the depth bound follows the same rule).
  //
  // invoke() runs these before creating a child frame; the test FFI mock frame
  // path enters at start_frame(), below invoke(), so it runs them itself
  // (#3301). The Invoke syscall body runs them before it copies the caller's
  // calldata, so a rejected invoke does no length-dependent host work (#2973)
  // -- which is why the depth bound is checked here and not further down in
  // invoke(), where it would refuse only after that copy.
  //
  // Failing is a no-op -- nothing here mutates the handler, so any of those
  // callers may ask more than once. The depth bound is a plain load off the
  // tip, which the caller must have pinned with init_db_tip() first.
  [[nodiscard]] SystemStatus<SyscallStatus<void>>
  validate_new_invocation(ContractAddress const &address, InvokeFlagSet flags,
                          std::span<uint32_t const> owned_auth_token_ids) const;

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
    return (*last_frame_).get_events();
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
  x23::Async<ExecResult> try_commit_transaction();

  // The two-phase-commit conflict diagnostic from the most recent failed commit
  // push (the root lock-acquire / 2pc push, or the commit_body_discarded RCQ /
  // replay path), at the DbDiagnosticLevel the database selected from the block
  // mode. The kDbDiagnosticNone arm if the last push succeeded. Valid after
  // try_commit_transaction() returns: abort_transaction() does not reset it.
  // Intended for SDK / test surfacing. Note: on outcomes that abort without a
  // commit push (System/Unrecoverable errors, or an expired-uniquifier
  // ValidationFailure), this may reflect an earlier in-body try_invoke conflict
  // rather than None — abort_transaction() does not reset it, so consumers
  // should key on the returned ExecResult (a genuine commit conflict is
  // ExecResultTxNonDeterministicError) before interpreting this value.
  DbConflictResult const &last_commit_conflict() const;

  x23::Async<SystemStatus<void>> abort_transaction();

  // Adds the replay-nonce consume delta for the root invoked address to the
  // current subtransaction batch (no-op when there is no uniquifier). The delta
  // rides in the same batch/push as the surrounding deltas, so a duplicate or
  // expired nonce makes that batch's reservation fail and the transaction is
  // rejected as a replay.
  x23::Async<void> add_replay_nonce_delta();

  // Commits a transaction whose body effects are discarded (a manual revert or
  // a deterministic fault). Adds validation-mode RCQ observation deltas and the
  // replay nonce to one batch, pushes it, then commits. A reservation conflict
  // (forged observation or replay) aborts the transaction and returns
  // ExecResultTxNonDeterministicError; otherwise returns success_result.
  x23::Async<ExecResult> commit_body_discarded(ExecResult success_result);
  // Emit the transaction's event log to the event server.
  // Called for all deterministic outcomes (OK, DeterministicError, Reverted).
  //
  // [proposed_tx] is the full transaction body forwarded to the event server
  // alongside the event log (assembled in proposal mode, forwarded in
  // validation mode).  Tests that only exercise the event-log path pass an
  // empty ProposedTransaction{} explicitly.
  x23::Async<SystemStatus<void>> emit_events(ExecResult exec_result,
                                             ProposedTransaction proposed_tx);

  // Get the proposal data.
  // Can only be called after root_invoke()
  // (as otherwise the proposal data is not available)
  NondeterministicProposalData const &get_proposal_data() const;

  // Syscalls: operations within an invocation

  template<auto... Flags, typename FlagSet>
  static constexpr bool validate_flags(FlagSet flags)
  {
    constexpr FlagSet kAllowedFlags = (0 | ... | std::to_underlying(Flags));
    return (flags & kAllowedFlags) == flags;
  }

  static constexpr bool validate_invoke_flags(InvokeFlagSet flags)
  {
    return validate_flags<InvokeFlag::TryInvoke, InvokeFlag::ReadOnly>(flags);
  }

  // Create and run a child call frame.
  // Should be called from within a syscall.
  //
  // The result layers the two failure channels:
  //  - outer SystemError: a block-ending error (Unrecoverable / Abandon /
  //    ValidationFailure).
  //  - inner SyscallError: the invoke was rejected before any child frame ran
  //    (kSyscallErrorCodeInvalidSyscallInput / kSyscallErrorCodeReentrance).
  //    This is a complete no-op.
  //  - value (InvocationFrameStatusCode): a child frame ran; the value is how
  //    it ended (Return / Abort / Fault).
  [[nodiscard]]
  x23::Async<std::pair<SystemStatus<SyscallStatus<InvocationFrameStatusCode>>,
                       GasAmount>>
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

  AuthClaim const &get_auth_claim() const;

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

  x23::Async<SystemStatus<SyscallStatus<ContractAddress>>>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  bool is_read_only() const
  {
    return invocation_frame_stack_.is_active_frame_read_only();
  }

  // === Crypto syscalls ===
  SyscallStatus<void> sha256_update(uint32_t input_index,
                                    std::span<const std::byte> input_buffer,
                                    bool input_reset);
  SyscallStatus<void> sha256_copy(uint32_t input_src_index,
                                  uint32_t input_dst_index);
  SyscallStatus<SHA256Hash> sha256_finalize(uint32_t input_index);

  // === Bytestring syscalls ===
  // The returned span is only valid until another storage item is accessed.
  x23::Async<SystemStatus<SyscallStatus<const Bytestring *>>>
  get_bytestring(StorageKeyContractData const &storage_key);
  x23::Async<SystemStatus<SyscallStatus<void>>>
  set_bytestring(StorageKeyContractData const &storage_key,
                 std::span<const std::byte> input_bytestring,
                 StorageDeltaFlagSet input_flags);

  // === Nonnegative Integer syscalls ===
  // get_nnint returns the base value and the add value.
  // Returns the base value from the snapshot if load_from_snapshot is true.
  // Otherwise, returns a tx-local written set value, if one exists,
  // or nullopt if not.
  x23::Async<
      SystemStatus<SyscallStatus<std::pair<std::optional<int128_t>, int128_t>>>>
  get_nnint(StorageKeyContractData const &storage_key, bool load_from_snapshot);

  x23::Async<SystemStatus<SyscallStatus<void>>>
  set_nnint(StorageKeyContractData const &storage_key,
            std::optional<int128_t> set_value, const int128_t &add_value,
            StorageDeltaFlagSet input_flags);

  // === Delete syscall ===
  x23::Async<SystemStatus<SyscallStatus<void>>>
  delete_object(StorageKeyContractData const &storage_key,
                StorageDeltaFlagSet flags);

  // === Retrocausal Queue syscalls ===
  x23::Async<SystemStatus<SyscallStatus<void>>>
  retrocausal_queue_insert(StorageKeyContractData const &key,
                           std::span<const std::byte> queue_entry);
  x23::Async<SystemStatus<SyscallStatus<QueueEntry>>>
  retrocausal_queue_consume(StorageKeyContractData const &key, bool peek);

  // == Native token syscalls ==
  x23::Async<SystemStatus<SyscallStatus<int128_t>>>
  get_native_token_balance(ContractAddress const &contract_address);
  x23::Async<SystemStatus<SyscallStatus<void>>>
  modify_native_token_balance(ContractAddress const &contract_address,
                              int128_t transfer_amount);

  // == Authentication syscalls ==
  x23::Async<SystemStatus<SyscallStatus<void>>>
  change_public_key_registration(uint32_t key_index,
                                 PublicKeyConfigOpt new_key);

  x23::Async<SystemStatus<SyscallStatus<void>>>
  set_address_required_weight(Weight required_weight);

  // Auth token syscalls
  SyscallStatus<uint32_t> mint_auth_token(AuthToken const &auth_token);
  SyscallStatus<InvocationFrameStack::ConsumeAuthTokenResult>
  consume_auth_token(uint32_t token_id, BlockHeight current_height);

  // == Block statistics syscalls ==
  // Served off the tip this handler pinned at entry (see init_db_tip()), so
  // reading one cannot fail and cannot touch the shared snapshot cache.
  BlockHeight get_block_height() const;
  NetworkHash const &get_network_hash() const;

  // Served from the BlockExecutionContext the block was opened with: for an
  // open block the tip is the parent, whose timepoint is the previous block's.
  TimePoint get_block_timepoint() const;

  // == General access methods ==
  //
  // returns nullptr if the contract does not exist
  x23::Async<SystemStatus<const Contract *>>
  resolve_contract(SHA256Hash const &hash) const;

  // Returns the image already built for `hash`: by perform_initial_validation
  // for an attachment, or by the block-scoped snapshot cache when it loaded an
  // already-committed contract. Never builds a second one. The result is
  // borrowed -- both owners are node-stable and outlive this transaction's
  // runtimes. Returns nullptr (wrapped) if `hash` names no contract.
  x23::Async<SystemStatus<const PreparedContract *>>
  resolve_prepared_contract(SHA256Hash hash);

  // == Error conversion methods ==
  static ExecResult to_exec_result(InvokeErrorCode error_code);
  static ExecResult to_exec_result(SystemErrorCode error_code);

  static auto force_commit_or_abort()
  {
    debug_force_commit_abort_ = true;
    return x23::Defer([]() { debug_force_commit_abort_ = false; });
  }

private:
  RuntimeContext &runtime_context_;
  PerTransactionDatabaseProxy db_proxy_;
  PerTransactionEventServerProxy event_server_proxy_;
  InvocationFrameStack invocation_frame_stack_;
  std::unique_ptr<InvocationFrameTrace> invocation_frame_trace_;

  const SHA256Hash source_tx_hash_;
  const AuthClaim auth_claim_;
  std::optional<Invocation> root_invocation_;

  // The transaction's replay-protection uniquifier and the root invoked address
  // it is consumed against. Both empty for the test-harness constructor (no
  // Transaction), in which case no replay nonce is consumed.
  const std::optional<Uniquifier> tx_uniquifier_;
  const std::optional<ContractAddress> root_invoked_address_;

  // Searchable map of contracts that are implicitly created by the current
  // transaction.
  const std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>,
                           SipHash24Hasher>
      newly_created_contracts_;

  // Noted at construction, where the attachment list is still in hand:
  // newly_created_contracts_ is keyed by hash, so a repeat leaves no trace
  // there. perform_initial_validation rejects it.
  const bool has_duplicate_attachment_ = false;

  // Images for this transaction's attachments, keyed by contract hash.
  // perform_initial_validation keeps what it built to verify each one rather
  // than building it again at first invocation (#3536); an already-committed
  // contract is served from the snapshot cache instead, so nothing is added
  // here later. Entries are node-stable and borrowed by the Runtimes built
  // from them, so they outlive those runtimes -- they live for the whole
  // transaction.
  std::unordered_map<SHA256Hash, PreparedContract, SipHash24Hasher>
      local_preparations_;

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

  // The block's db tip, pinned on this handler at the transaction's first
  // admission check (see init_db_tip()). Owned by snapshot_cache_ and stable
  // for the whole block, so every later read of the executing block height,
  // the network configuration, or the default wallet is a plain load off a
  // thread-confined pointer -- no atomic, no fallible path, and no reason for
  // a caller to reach into the shared cache while holding one of its locks.
  //
  // Mutable because it memoizes an immutable value: the const admission checks
  // that run before any frame is pushed are what pin it.
  mutable DbTip const *db_tip_ = nullptr;

  // Flattened when the tip is pinned: the invoke path reads these per frame.
  mutable NetworkConfigParams config_params_{};

  inline static bool debug_force_commit_abort_{false};

  // The block's first tip fetch, split out so init_db_tip() can answer an
  // already-pinned handler from MaybeAsync's value arm. Also re-checks the
  // opened height against the tip, the one place that can fail.
  [[nodiscard]] x23::Async<SystemStatus<void>> pin_db_tip() const;

  // The pinned tip. Every caller runs after an admission check that pinned it,
  // so a null here is a missed pin point on a new entry path, not a runtime
  // condition -- fail loudly rather than reading through null.
  DbTip const *db_tip() const
  {
    CHECK_PTR(db_tip_);
    return db_tip_;
  }

  // The parameters this transaction's chain puts in force. Pinned with the tip.
  NetworkConfigParams const &config_params() const
  {
    CHECK_PTR(db_tip_);
    return config_params_;
  }

  // True if the given replay uniquifier has already expired at the executing
  // block height -- i.e. the HogDb replay cache would reject it as a conflict.
  static bool replay_uniquifier_expired(Uniquifier const &uniquifier,
                                        BlockHeight executing_height);
  // Whether this handler's transaction has an already-expired replay uniquifier
  // (reads the executing height off the pinned tip). Returns false for the
  // test-harness constructor (no uniquifier). Used by
  // perform_initial_validation() to reject the transaction before the body
  // runs, and by try_commit_transaction()'s debug-only check that a
  // proposal-mode ValidationFailure is in fact a bad expiry height.
  bool replay_uniquifier_expired() const;

  // Validate that the transaction can be run safely.
  // Ensures, for example, that the wasms are valid, and that the replay
  // uniquifier has not already expired.
  // Sets cumulative_invoke_result_ to the appropriate error on a deterministic
  // validation failure; returns a system error (e.g. an unreadable db tip)
  // through the SystemStatus so the caller can surface it rather than drop it.
  x23::Async<SystemStatus<void>> perform_initial_validation();

  // check the validation data at the end of a transaction, if in validation
  // mode
  void check_final_validation_data();

  // Convert the HostFnStatus returned from the root invocation frame into an
  // InvokeStatus for the top level return.
  static InvokeStatus<void> handle_root_host_fn_status(
      SystemStatus<SyscallStatus<InvocationFrameStatusCode>> status);

  // Resolve the WasmiRuntime associated with the queried address.
  // Creates a new runtime if contract is deployed at
  // that address in the current context (i.e. if a contract is deployed there
  // in the snapshot, or by a prior action of the current transaction that was
  // not rolled back by a revert).
  x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  resolve_runtime(ContractAddress const &addr);

  // Commit the last call frame, if the last call frame succeeds.
  // Extracts relevant data structures, to prepare for committing the
  // transaction.
  void commit_last_call_frame_success(InvocationFrame &&frame);

  // On a failed 2pc push: trace-logs db_proxy_'s last_push_conflict() and
  // fire-and-forgets a best-effort TxFailureDiagnostic (first conflict +
  // executing block height) to the event server, which relays it to the
  // indexers so clients can learn why the tx was excluded (see
  // docs/superpowers/specs/2026-07-07-tx-failure-diagnostics-cache-design.md).
  // A no-op when hog_db captured no diagnostics (kDbDiagnosticNone, e.g.
  // validation/viewing blocks) -- hog_db selects the level from the block's
  // execution mode, so there is no seam to configure here. Called right
  // after each 2pc push failure site sets two_phase_commit_failed_. Never
  // fails; suspends only on the db-tip read the diagnostic's attempt height
  // needs, and the send itself stays fire-and-forget.
  x23::Async<void> report_db_conflict_diagnostic() const;

  BlockExecutionMode get_block_execution_mode() const
  {
    if (std::holds_alternative<ProposalModeData>(execution_mode_data_)) {
      return BlockExecutionMode::kBlockExecutionModeProposal;
    }
    if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
      return BlockExecutionMode::kBlockExecutionModeValidation;
    }
    assert(std::holds_alternative<ViewingModeData>(execution_mode_data_));
    return BlockExecutionMode::kBlockExecutionModeViewing;
  }
};

} // namespace hogd
