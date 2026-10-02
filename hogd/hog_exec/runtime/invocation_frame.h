#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "auth_token.hh"
#include "contract.hh"
#include "db_conflict.hh"
#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/auth_token_registry.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/components/hash_buffers.h"
#include "hogd/hog_exec/runtime/item/frame_item_key.h"
#include "hogd/hog_exec/runtime/item/item_view.h"
#include "hogd/hog_exec/runtime/validated_auth_tokens.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

class PerTransactionDatabaseProxy;
struct InvokeResult;

// Stores all relevant data structures for one Invocation in a transaction.
class InvocationFrame {
public:
  InvocationFrame(ContractAddress const &address, Calldata &&calldata,
                  InvokeFlagSet flags, size_t event_count,
                  AuthTokenIdSet &&owned_auth_token_ids,
                  uint32_t inherited_try_invoke_depth,
                  NetworkConfigParams const &params);

  // Disable copy construction/assignment but allow move ctor
  InvocationFrame(InvocationFrame const &) = delete;
  InvocationFrame &operator=(InvocationFrame const &) = delete;

  InvocationFrame(InvocationFrame &&) noexcept = default;
  InvocationFrame &operator=(InvocationFrame &&) noexcept = delete;

  ContractAddress const &address() const { return address_; }
  Calldata const &calldata() const { return calldata_; }

  // The value of the last _child_ call frame's return value.
  // Set to nullopt before any child call happens.
  std::optional<InvocationFrameOutput> child_return_output_ = std::nullopt;
  std::vector<uint32_t> child_returned_auth_token_ids_;

  // merge child invocation frame into self
  void commit_successful_child_frame(InvocationFrame &&frame);

  // merge child invocation frame into self
  void commit_manually_reverted_frame(InvocationFrame &&frame);

  // revert the child invocation frame.
  // Does not clean up the child frame, but is responsible for handling the
  // parent frame's response to the child frame's failure.
  void commit_failed_child_frame();

  // Mark the current frame as failed.
  // Used to clean up failed try_invoke frames.
  [[nodiscard]] x23::Async<SystemStatus<void>>
  mark_frame_as_failed(PerTransactionDatabaseProxy &db_proxy) const;

  // set the value returned from the current call frame.
  void set_frame_return(InvocationFrameOutput &&return_output,
                        std::vector<uint32_t> &&returned_auth_token_ids = {});

  // add a log message to the output of the transaction, associated with the
  // current call frame.
  void log_event(TxEmittedEvent event);
  size_t event_count() const { return event_count_; }

  x23::Vec<TxEmittedEvent> const &get_events() const { return events_; }

  x23::Vec<TxEmittedEvent> extract_events()
  {
    auto out = std::move(events_);
    events_.clear();
    return out;
  }

  bool is_try_invoke() const
  {
    return (flags_ & std::to_underlying(InvokeFlag::TryInvoke)) != 0;
  }

  bool is_read_only() const
  {
    return (flags_ & std::to_underlying(InvokeFlag::ReadOnly)) != 0;
  }

  uint32_t get_total_try_invoke_depth() const
  {
    return total_try_invoke_depth_;
  }

  // Number of HogDb subtransactions this frame has pushed for its own
  // successful TryInvoke children. Validation mode never pushes per-frame
  // subtransactions, so this stays 0 on every frame in validation; useful
  // for asserting that invariant at the root commit site.
  uint32_t get_try_invoke_frame_count() const
  {
    return try_invoke_frame_count_;
  }

  bool depth_cap_reached(NetworkConfigParams const &params) const
  {
    // One slot is reserved for the root commit's subtransaction push; a depth
    // of 0 leaves none for a TryInvoke.
    return params.max_savepoint_depth == 0 ||
           total_try_invoke_depth_ >= params.max_savepoint_depth - 1;
  }

  // returned pointer is invalidated by any call to add_item_view
  // Does not create a new view if it does not exist
  auto *get_item_view(this auto &self, FrameItemKey const &key)
  {
    auto it = self.modified_items_.find(key);
    if (it != self.modified_items_.end()) {
      return &(it->second);
    }
    return static_cast<decltype(&(it->second))>(nullptr);
  }
  // returns pointer to the newly created view
  ItemView *add_item_view(FrameItemKey const &key, ItemView new_view);

  [[nodiscard]] x23::Async<SystemStatus<bool>> try_acquire_frame_locks(
      PerTransactionDatabaseProxy &db_proxy,
      std::map<AddressAndKey, std::vector<QueueEntry>> const &observations);

  // Records a successful child TryInvoke against the depth cap. finish_frame
  // calls this once a child's success is settled in any execution mode, so
  // the cap counts the same frames whether the mode probed locks (proposal),
  // replayed a bit (validation), or defaulted one (viewing). Precondition:
  // !depth_cap_reached(params).
  void note_try_invoke_success(NetworkConfigParams const &params);

  void
  collect_storage_deltas(x23::Vec<FlaggedStorageDelta, 0xffffffff> &out) const;

  bool should_revert() const
  {
    switch (to_return_output_.status_code()) {
    case InvocationFrameStatusCode::Return:
      return false;
    case InvocationFrameStatusCode::Fault:
    case InvocationFrameStatusCode::Abort:
      return true;
    }
    panic("Unreachable");
  }

  InvocationFrameOutput const &get_frame_output() const
  {
    return to_return_output_;
  }

  void deploy_contract(ContractAddress const &addr,
                       DeployedContractHash const &hash)
  {
    [[maybe_unused]] auto [_, inserted] =
        new_contract_deployments_.emplace(addr, hash);
    assert(inserted);
  }

  std::optional<DeployedContractHash>
  lookup_locally_deployed_contract(ContractAddress const &addr) const
  {
    auto it = new_contract_deployments_.find(addr);
    if (it != new_contract_deployments_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  std::unordered_set<SHA256Hash, SipHash24Hasher>
  get_committed_contracts() const
  {
    std::unordered_set<SHA256Hash, SipHash24Hasher> committed_contracts;
    committed_contracts.reserve(new_contract_deployments_.size());
    for (auto const &[addr, deployment] : new_contract_deployments_) {
      committed_contracts.insert(deployment.hash);
    }
    return committed_contracts;
  }

  bool check_owns_auth_token(uint32_t token_id) const
  {
    return owned_auth_token_ids_.contains(token_id);
  }

  void mint_auth_token(uint32_t token_id)
  {
    owned_auth_token_ids_.insert(token_id);
  }

  void remove_auth_tokens(std::span<uint32_t const> token_ids)
  {
    for (auto token_id : token_ids) {
      owned_auth_token_ids_.erase(token_id);
    }
  }

  HashBuffers hash_buffers_;

  // Ids stay in this frame's owned set until the child commits successfully.
  void
  set_child_forwarded_auth_tokens(std::vector<uint32_t> &&token_ids) noexcept
  {
    assert(child_forwarded_auth_tokens_.empty());
    child_forwarded_auth_tokens_ = std::move(token_ids);
  }

  std::span<const uint32_t> get_child_forwarded_auth_tokens() const
  {
    return child_forwarded_auth_tokens_;
  }

  void
  set_returned_auth_token_ids(std::vector<uint32_t> &&returned_auth_token_ids)
  {
    returned_auth_token_ids_ = std::move(returned_auth_token_ids);
  }

private:
  ContractAddress address_;
  Calldata calldata_;

  // buffer to return to parent
  InvocationFrameOutput to_return_output_ =
      InvocationFrameOutput{InvocationFrameStatusCode::Return};
  std::vector<uint32_t> returned_auth_token_ids_;

  // flags on the invocation
  const InvokeFlagSet flags_;

  // Accumulates the events created in the current call frame (and child frames)
  x23::Vec<TxEmittedEvent> events_;
  size_t event_count_ = 0;

  uint32_t try_invoke_frame_count_ = 0;
  uint32_t total_try_invoke_depth_;

  // elements modified during the current call frame (and child frames)
  // Keyed by the flat, pre-hashed form: a first touch probes every open frame
  // with one key, and the hash is computed once for all of them.
  std::unordered_map<FrameItemKey, ItemView, FrameItemKeyHash> modified_items_;

  // newly deployed contracts during the current call frame (and child frames)
  std::unordered_map<ContractAddress, DeployedContractHash, SipHash24Hasher>
      new_contract_deployments_;

  AuthTokenIdSet owned_auth_token_ids_;
  // Forwarded to the running child; leaves owned_auth_token_ids_ only when
  // that child commits successfully.
  std::vector<uint32_t> child_forwarded_auth_tokens_;
};

static_assert(std::is_nothrow_move_constructible_v<InvocationFrame>);

// Stack of invocation frames.
class InvocationFrameStack {
public:
  // `config_params` are the pinned tip's; one reference serves every frame.
  InvocationFrameStack(
      std::function<void(InvocationFrame &&)> last_frame_action,
      PerTransactionDatabaseProxy &db_proxy,
      NetworkConfigParams const &config_params)
    : last_frame_action_(std::move(last_frame_action)), db_proxy_(db_proxy),
      config_params_(config_params)
  {}

  ContractAddress const &get_current_address() const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().address();
  }

  void
  set_active_frame_return(InvocationFrameOutput &&return_output,
                          std::vector<uint32_t> &&returned_auth_token_ids = {})
  {
    assert(!frame_stack_.empty());
    frame_stack_.back().set_frame_return(std::move(return_output),
                                         std::move(returned_auth_token_ids));
  }

  bool empty() const { return frame_stack_.empty(); }
  size_t event_count() const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().event_count();
  }

  // The tokens MUST be owned by the parent frame; the caller checks. Consumes
  // the list; the parent's owned set is untouched until the child commits.
  // Admission failure is a clean no-op that returns Unrecoverable (#3058).
  [[nodiscard]] SystemStatus<void>
  start_frame(ContractAddress const &address, Calldata &&calldata,
              InvokeFlagSet flags, ValidatedAuthTokens &&owned_auth_tokens);
  // Returns a pair of the InvokeResult and a bool indicating whether the frame
  // was manually reverted.
  // If try_invoke_conflict_out is non-null and this frame is a proposal-mode
  // try_invoke, it is set to the frame's post-acquire two-phase-commit conflict
  // (kDbDiagnosticNone on a successful acquire).
  //
  // On allocation failure while committing the child into its parent, the child
  // is still popped and an Unrecoverable SystemError is returned, so the
  // noexcept caller (SystemCallHandler::finish_frame) is never terminated
  // (#3378).
  [[nodiscard]] x23::Async<SystemStatus<std::pair<InvokeResult, bool>>>
  finish_frame(InvokeStatus<void> result,
               ExecutionModeData &execution_mode_data,
               DbConflictResult *try_invoke_conflict_out = nullptr);

  void log_trace_finish(InvocationFrameTrace &trace);

  std::optional<ContractAddress> get_msg_sender() const;

  bool has_active_frame_for(ContractAddress const &addr) const
  {
    return std::ranges::any_of(frame_stack_, [&](const InvocationFrame &frame) {
      return frame.address() == addr;
    });
  }

  bool is_active_frame_read_only() const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().is_read_only();
  }

  x23::Vec<TxEmittedEvent> copy_active_frame_events() const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().get_events();
  }

  HashBuffers &get_active_frame_hash_buffers()
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().hash_buffers_;
  }

  std::optional<InvocationFrameOutput> const &
  get_active_frame_child_return_output() const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().child_return_output_;
  }

  std::span<const uint32_t>
  get_active_frame_child_returned_auth_token_ids() const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().child_returned_auth_token_ids_;
  }

  const Calldata &get_active_frame_calldata() const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().calldata();
  }

  std::span<const uint32_t>
  get_current_frame_parent_supplied_auth_tokens() const
  {
    assert(!frame_stack_.empty());
    if (frame_stack_.size() < 2) {
      return std::span<const uint32_t>();
    }
    return frame_stack_[frame_stack_.size() - 2]
        .get_child_forwarded_auth_tokens();
  }

  void log_event(TxEmittedEvent event)
  {
    assert(!frame_stack_.empty());
    assert(event.contract_address == frame_stack_.back().address());
    frame_stack_.back().log_event(std::move(event));
  }

  void deploy_contract(ContractAddress const &addr,
                       DeployedContractHash const &hash)
  {
    assert(!frame_stack_.empty());
    frame_stack_.back().deploy_contract(addr, hash);
  }

  // Lookup the hash of the contract deployed at the queried address,
  // _only_ for contracts that are deployed during the current transaction.
  // Allows deployments to be created and then invoked during one transaction
  // (useful for initialization).
  //
  // Also usable to check whether a deployment was rolled back by a reverted
  // call frame.
  std::optional<DeployedContractHash>
  lookup_locally_deployed_contract(ContractAddress const &addr) const;

  // Reference invalidated by any other call to get_item_view
  // The returned pointer is never null
  ItemView *get_item_view(StorageKeyContractData const &data_key);
  ItemView *get_item_view(AddressAndKey const &addrkey);
  ItemView *get_item_view(FrameItemKey const &key);

  // Reads the storage flags for `key` without materializing a view in the
  // active frame. Walks the frame stack from the active frame outward and
  // returns the first matching view's flags, or None when no frame holds the
  // key. Unlike get_item_view, a metadata-only flags read never copies a
  // parent frame's (possibly large) item into the active frame.
  StorageDeltaFlagSet get_item_view_flags(FrameItemKey const &key) const;
  StorageDeltaFlagSet
  get_item_view_flags(StorageKeyContractData const &data_key) const;

  // Whether the active (top) frame already holds a materialized view for
  // `key`, without walking parent frames or materializing one.
  bool active_frame_holds_item_view(FrameItemKey const &key) const
  {
    assert(!frame_stack_.empty());
    return frame_stack_.back().get_item_view(key) != nullptr;
  }

  // The frame key for `data_key` under the active frame's address, laid out
  // and hashed once. A syscall that may reach the snapshot cache builds this,
  // probes the frames with it, and hands the same key to UnmodifiedItem::load,
  // so the key is hashed once per syscall rather than once per structure.
  FrameItemKey frame_item_key(StorageKeyContractData const &data_key) const
  {
    assert(!frame_stack_.empty());
    return FrameItemKey::contract_data(frame_stack_.back().address(), data_key);
  }

  AddressAndKey get_addrkey(StorageKeyContractData const &data_key) const
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = data_key;
    return AddressAndKey{.address = frame_stack_.back().address(), .key = key};
  }

  AuthToken const *lookup_auth_token(uint32_t token_id) const;
  [[nodiscard]] SyscallStatus<uint32_t>
  mint_auth_token(AuthToken const &auth_token);

  struct ConsumeAuthTokenResult {
    AuthTokenPayload const *payload;
    ContractAddress const *mint_address;
    uint32_t new_token_id_start;
    uint32_t new_token_id_count;
  };
  [[nodiscard]] SyscallStatus<ConsumeAuthTokenResult>
  consume_auth_token(uint32_t token_id);

  bool check_auth_tokens_owned(std::span<uint32_t const> token_ids) const;
  bool check_auth_tokens_owned(AuthTokenIdSet const &token_ids) const;

  size_t size() const { return frame_stack_.size(); }

private:
  std::vector<InvocationFrame> frame_stack_;
  std::function<void(InvocationFrame &&)> last_frame_action_;
  PerTransactionDatabaseProxy &db_proxy_;
  NetworkConfigParams const &config_params_;
  AuthTokenRegistry auth_token_registry_;
};

} // namespace hogd
