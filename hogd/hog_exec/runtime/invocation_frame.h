#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
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
#include "hogd/hog_exec/components/auth_subtoken.h"
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
                  std::vector<RegisteredAuthToken> &&forwarded_auth_tokens,
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

  // Auth token ids are local to a frame: a token's id is its index in
  // auth_tokens_. A frame's forwarded tokens are its ids 0..N-1, in the order
  // its parent listed them, and every token minted, unwrapped or returned to
  // the frame takes the next id.
  bool check_owns_auth_token(uint32_t token_id) const
  {
    return token_id < auth_tokens_.size() &&
           static_cast<bool>(auth_tokens_[token_id].root_node);
  }

  // Null if this frame does not own token_id. Invalidated by add_auth_token.
  RegisteredAuthToken const *lookup_auth_token(uint32_t token_id) const
  {
    return check_owns_auth_token(token_id) ? &auth_tokens_[token_id] : nullptr;
  }

  // The id the next added token takes.
  size_t next_auth_token_id() const { return auth_tokens_.size(); }

  // Adds token under the next id and returns it, or nullopt once the ids are
  // exhausted.
  std::optional<uint32_t> add_auth_token(RegisteredAuthToken token)
  {
    if (auth_tokens_.size() >= std::numeric_limits<uint32_t>::max()) {
      return std::nullopt;
    }
    auth_tokens_.push_back(std::move(token));
    return static_cast<uint32_t>(auth_tokens_.size() - 1);
  }

  // Removes a token this frame owns, and returns it.
  RegisteredAuthToken take_auth_token(uint32_t token_id)
  {
    assert(check_owns_auth_token(token_id));
    return std::exchange(auth_tokens_[token_id], RegisteredAuthToken{});
  }

  void remove_auth_tokens(std::span<uint32_t const> token_ids)
  {
    for (auto token_id : token_ids) {
      take_auth_token(token_id);
    }
  }

  // This frame's ids for the tokens its parent forwarded: 0..N-1.
  std::span<const uint32_t> get_parent_supplied_auth_token_ids() const
  {
    return parent_supplied_auth_token_ids_;
  }

  HashBuffers hash_buffers_;

  // The forwarded tokens stay in this frame until the child commits
  // successfully.
  void
  set_child_forwarded_auth_tokens(std::vector<uint32_t> &&token_ids) noexcept
  {
    assert(child_forwarded_auth_tokens_.empty());
    child_forwarded_auth_tokens_ = std::move(token_ids);
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

  // This frame's auth tokens, indexed by id. A token that has left the frame
  // (consumed, or forwarded to a child that committed) leaves an empty entry,
  // so ids are never reused.
  std::vector<RegisteredAuthToken> auth_tokens_;
  std::vector<uint32_t> parent_supplied_auth_token_ids_;
  // This frame's ids for the tokens forwarded to the running child; they leave
  // auth_tokens_ only when that child commits successfully.
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
    return frame_stack_.back().get_parent_supplied_auth_token_ids();
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

  RegisteredAuthToken const *lookup_auth_token(uint32_t token_id) const;
  [[nodiscard]] SyscallStatus<uint32_t>
  mint_auth_token(RegisteredAuthToken token);

  // The consumed token, and the ids its children took in the consuming frame.
  struct ConsumeAuthTokenResult {
    RegisteredAuthToken token;
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
};

} // namespace hogd
