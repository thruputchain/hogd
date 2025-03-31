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

#include "hogd/hogexec/runtime/item_view.h"
#include "transaction.hh"
#include "types.hh"

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

  InvocationFrame(Invocation invocation);

  // merge child invocation frame into self
  void commit_successful_child_frame(InvocationFrame &frame);
  // revert the child invocation frame
  void commit_failed_child_frame();

  // set the value returned from the current call frame.
  void set_frame_return(ReturnBuffer &&return_buffer);

  // add a log message to the output of the transaction, associated with the
  // current call frame.
  void log(std::span<const std::byte> message);
  std::vector<LogEntry> extract_logs() { return std::move(logs_); }

  // returned pointer is invalidated by any call to add_item_view
  ItemView *get_item_view(AddressAndKey const &addrkey);
  // returns pointer to the newly created view
  ItemView *add_item_view(AddressAndKey const &addrkey, ItemView new_view);

private:
  // buffer (bytestring) to return to parent
  ReturnBuffer to_return_;

  // Accumulates log entries created in the current call frame (and child
  // frames)
  std::vector<LogEntry> logs_;

  std::unordered_map<AddressAndKey, ItemView> modified_items_;
};

// Stack of invocation frames.
class InvocationFrameStack {
public:
  InvocationFrameStack(std::function<void(InvocationFrame &)> last_frame_action, StorageSnapshotCache& snapshot_cache)
    : last_frame_action_(std::move(last_frame_action)),
    snapshot_cache_(snapshot_cache) {}

  auto &get_active_frame(this auto &self)
  {
    assert(!self.frame_stack_.empty());
    return self.frame_stack_.back();
  }
  bool empty() const { return frame_stack_.empty(); }

  void start_frame(Invocation const &invocation);
  void finish_frame(InvokeResult const &result);

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
  ItemView &get_item_view(StorageKey const &key);

private:
  std::vector<InvocationFrame> frame_stack_;
  std::function<void(InvocationFrame &)> last_frame_action_;
  StorageSnapshotCache& snapshot_cache_;
};

} // namespace hogd
