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
#include "hog_exec_constants.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/auth_token_registry.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/components/hash_buffers.h"
#include "hogd/hog_exec/runtime/item/item_view.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class PerTransactionDatabaseProxy;
struct InvokeResult;

// Stores all relevant data structures for one Invocation in a transaction.
class InvocationFrame {
public:
  InvocationFrame(ContractAddress const &address, Calldata &&calldata,
                  InvokeFlagSet flags, size_t event_count,
                  std::span<uint32_t const> owned_auth_token_ids,
                  uint32_t inherited_try_invoke_depth);

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
  SystemStatus<void>
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

  bool depth_cap_reached() const
  {
    // Reserve one savepoint slot for the root commit frame, which always
    // pushes a subtransaction through the same HogDb path.
    return total_try_invoke_depth_ >= kMaxSavepointDepth - 1;
  }

  // returned pointer is invalidated by any call to add_item_view
  // Does not create a new view if it does not exist
  auto *get_item_view(this auto &self, AddressAndKey const &addrkey)
  {
    auto it = self.modified_items_.find(addrkey);
    if (it != self.modified_items_.end()) {
      return &(it->second);
    }
    return static_cast<decltype(&(it->second))>(nullptr);
  }
  // returns pointer to the newly created view
  ItemView *add_item_view(AddressAndKey const &addrkey, ItemView new_view);

  SystemStatus<bool> try_acquire_frame_locks(
      PerTransactionDatabaseProxy &db_proxy,
      std::map<AddressAndKey, std::vector<QueueEntry>> const &observations);

  // Validation-mode counterpart for a successful TryInvoke. Bumps the
  // intra-transaction depth counter without pushing a HogDb subtransaction
  // (validation does not use per-frame subtransactions). Precondition:
  // !depth_cap_reached().
  void note_validation_try_invoke_success();

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

  std::unordered_set<SHA256Hash, ByteArrayHasher>
  get_committed_contracts() const
  {
    std::unordered_set<SHA256Hash, ByteArrayHasher> committed_contracts;
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

  void set_child_restore_auth_tokens(std::span<uint32_t const> token_ids)
  {
    assert(most_recent_child_frame_forwarded_auth_tokens_.empty());
    most_recent_child_frame_forwarded_auth_tokens_.reserve(token_ids.size());
    for (auto token_id : token_ids) {
      most_recent_child_frame_forwarded_auth_tokens_.push_back(token_id);
    }
  }

  std::span<const uint32_t> get_child_forwarded_auth_tokens() const
  {
    return most_recent_child_frame_forwarded_auth_tokens_;
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
  std::unordered_map<AddressAndKey, ItemView> modified_items_;

  // newly deployed contracts during the current call frame (and child frames)
  std::unordered_map<ContractAddress, DeployedContractHash, ByteArrayHasher>
      new_contract_deployments_;

  std::unordered_set<uint32_t> owned_auth_token_ids_;
  std::vector<uint32_t>
      most_recent_child_frame_forwarded_auth_tokens_; // refunded if the frame
                                                      // reverts
};

static_assert(std::is_nothrow_move_constructible_v<InvocationFrame>);

// Stack of invocation frames.
class InvocationFrameStack {
public:
  InvocationFrameStack(
      std::function<void(InvocationFrame &&)> last_frame_action,
      PerTransactionDatabaseProxy &db_proxy)
    : last_frame_action_(std::move(last_frame_action)), db_proxy_(db_proxy)
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

  // It MUST be the case that the owned_auth_token_ids are owned by the parent
  // frame. Caller is responsible for checking as much.
  //
  // Admitting a child frame runs throwing STL allocations. On allocation
  // failure the call is a clean no-op — any parent state touched is rolled
  // back — and an Unrecoverable SystemError is returned, so the noexcept caller
  // (SystemCallHandler::start_frame) never lets the exception terminate the
  // executor (#3058).
  SystemStatus<void> start_frame(ContractAddress const &address,
                                 Calldata &&calldata, InvokeFlagSet flags,
                                 std::span<uint32_t const> owned_auth_token_ids);
  // Returns a pair of the InvokeResult and a bool indicating whether the frame
  // was manually reverted.
  // If try_invoke_conflict_out is non-null and this frame is a proposal-mode
  // try_invoke, it is set to the frame's post-acquire two-phase-commit conflict
  // (kDbDiagnosticNone on a successful acquire).
  SystemStatus<std::pair<InvokeResult, bool>>
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

  AddressAndKey get_addrkey(StorageKeyContractData const &data_key) const
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = data_key;
    return AddressAndKey{.address = frame_stack_.back().address(), .key = key};
  }

  AuthToken const *lookup_auth_token(uint32_t token_id) const;
  SyscallStatus<uint32_t> mint_auth_token(AuthToken const &auth_token);

  struct ConsumeAuthTokenResult {
    AuthTokenPayload const *payload;
    ContractAddress const *mint_address;
    uint32_t new_token_id_start;
    uint32_t new_token_id_count;
  };
  SyscallStatus<ConsumeAuthTokenResult> consume_auth_token(uint32_t token_id);

  bool check_auth_tokens_owned(std::span<uint32_t const> token_ids) const;

  size_t size() const { return frame_stack_.size(); }

private:
  std::vector<InvocationFrame> frame_stack_;
  std::function<void(InvocationFrame &&)> last_frame_action_;
  PerTransactionDatabaseProxy &db_proxy_;
  AuthTokenRegistry auth_token_registry_;
};

} // namespace hogd
