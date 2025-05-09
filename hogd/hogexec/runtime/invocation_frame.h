#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/status.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

struct InvokeResult;
class StorageSnapshotCache;

// Stores all relevant data structures for one Invocation in a transaction.
class InvocationFrame {
public:
  // the invocation that started this call frame
  const Invocation active_invocation_;

  // new deployments that were created during this (and child) call frames
  std::map<ContractAddress, SHA256Hash> new_contract_deployments_;

  // The value of the last _child_ call frame's returned string.
  // Set to nullopt on failed calls, or before any child call happens.
  std::optional<ReturnBuffer> child_return_buffer_ = std::nullopt;

  InvocationFrame(Invocation invocation, InvokeFlagSet flags);

  // merge child invocation frame into self
  void commit_successful_child_frame(InvocationFrame &&frame);

  // revert the child invocation frame.
  // Does not clean up the child frame, but is responsible for handling the parent
  // frame's response to the child frame's failure.
  void commit_failed_child_frame();

  // Mark the current frame as failed.
  // Used to clean up failed try_invoke frames.
  SystemStatus<void> mark_frame_as_failed(PerTransactionDatabaseProxy &db_proxy) const;

  // set the value returned from the current call frame.
  void set_frame_return(ReturnBuffer &&return_buffer);

  // add a log message to the output of the transaction, associated with the
  // current call frame.
  void log(std::span<const std::byte> message);
  std::vector<LogEntry> extract_logs() { return std::move(logs_); }
  std::unordered_map<AddressAndKey, ItemView> extract_modified_items()
  {
    return std::move(modified_items_);
  }

  bool is_try_invoke() const
  {
    return (flags_ & std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke)) != 0;
  }

  // returned pointer is invalidated by any call to add_item_view
  ItemView *get_item_view(AddressAndKey const &addrkey);
  // returns pointer to the newly created view
  ItemView *add_item_view(AddressAndKey const &addrkey, ItemView new_view);

  x23::Async<SystemStatus<bool>>
  try_acquire_frame_locks(PerTransactionDatabaseProxy &db_proxy);

private:
  // buffer (bytestring) to return to parent
  ReturnBuffer to_return_;

  // flags on the invocation
  const InvokeFlagSet flags_;

  // Accumulates log entries created in the current call frame (and child
  // frames)
  std::vector<LogEntry> logs_;

  uint32_t try_invoke_frame_count_ = 0;

  // elements modified during the current call frame (and child frames)
  std::unordered_map<AddressAndKey, ItemView> modified_items_;
};

// Stack of invocation frames.
class InvocationFrameStack {
public:
  InvocationFrameStack(
      std::function<void(InvocationFrame &&)> last_frame_action,
      StorageSnapshotCache &snapshot_cache,
      PerTransactionDatabaseProxy &db_proxy)
    : last_frame_action_(std::move(last_frame_action)),
      snapshot_cache_(snapshot_cache), db_proxy_(db_proxy)
  {}

  auto &get_active_frame(this auto &self)
  {
    assert(!self.frame_stack_.empty());
    return self.frame_stack_.back();
  }
  bool empty() const { return frame_stack_.empty(); }

  SystemStatus<void> start_frame(Invocation const &invocation, InvokeFlagSet flags);
  SystemStatus<void> finish_frame(InvokeResult result,
                                  ExecutionModeData &execution_mode_data);

  std::optional<ContractAddress> get_msg_sender() const;

  // Lookup the hash of the contract deployed at the queried address,
  // _only_ for contracts that are deployed during the current transaction.
  // Allows deployments to be created and then invoked during one transaction
  // (useful for initialization).
  //
  // Also usable to check whether a deployment was rolled back by a reverted
  // call frame. (even if a corresponding runtime entry exists).
  std::optional<SHA256Hash>
  lookup_locally_deployed_contract(ContractAddress const &addr) const;

  // Reference invalidated by any other call to get_item_view
  // The returned pointer is never null
  SystemStatus<ItemView *> get_item_view(StorageKey const &key);

private:
  std::vector<InvocationFrame> frame_stack_;
  std::function<void(InvocationFrame &&)> last_frame_action_;
  StorageSnapshotCache &snapshot_cache_;
  PerTransactionDatabaseProxy &db_proxy_;
};

} // namespace hogd
