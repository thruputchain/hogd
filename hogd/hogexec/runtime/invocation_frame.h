#pragma once

#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "transaction.hh"

namespace hogd {

struct RuntimeCacheEntry;
struct InvokeResult;
class StorageSnapshotCache;

// Stores all relevant data structures for one Invocation in a transaction.
class InvocationFrame {
public:
  // the invocation that started this call frame
  const Invocation active_invocation_;

  // new deployments that were created during this (and child) call frames
  std::unordered_map<ContractAddress, SHA256Hash> new_contract_deployments_;

  // The value of the last _child_ call frame's returned string.
  // Set to nullopt on failed calls, or before any child call happens.
  std::optional<ReturnBuffer> child_return_buffer_ = std::nullopt;

  InvocationFrame(Invocation invocation, RuntimeCacheEntry *visited_entry,
                  InvocationFlagSet flags);

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
  std::unordered_map<AddressAndKey, FlaggedStorageDelta> extract_deltas();
  std::unordered_map<ContractAddress, SHA256Hash> extract_deployments()
  {
    return std::move(new_contract_deployments_);
  }

  void poison_visited_runtimes();

  // returned pointer is invalidated by any call to add_item_view
  ItemView *get_item_view(AddressAndKey const &addrkey);
  void add_item_view(AddressAndKey const &addrkey, ItemView new_view);

  // try invoke related methods
  bool is_try_invoke_frame() const
  {
    return (flags_ & std::to_underlying(InvocationFlags::kTryInvoke)) != 0;
  }
  bool handle_try_acquire_locks(PerTransactionDatabaseProxy &db_proxy);

private:
  // buffer (bytestring) to return to parent
  ReturnBuffer to_return_;

  std::vector<LogEntry> logs_;

  // The set of all runtimes visited by the current call frame
  // and _successful_ children. Failed calls poison the set of visited
  // runtimes, unless the runtime is marked UNWIND_SAFE (not this PR) Failed
  // call frames immediately poison their set of visited runtimes.
  std::unordered_set<RuntimeCacheEntry *> visited_runtimes_;

  std::unordered_map<AddressAndKey, ItemView> modified_items_;

  InvocationFlagSet flags_;
};

// Stack of invocation frames.
class InvocationFrameStack {
public:
  InvocationFrameStack(std::function<void(InvocationFrame &)> last_frame_action,
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

  void start_frame(Invocation const &invocation,
                   RuntimeCacheEntry *visited_entry, InvocationFlagSet flags);
  [[nodiscard]] InvokeResult
  finish_frame(InvokeResult const &result,
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

  // refreshes the itemview into top call frame
  // Reference is invalidated by any other call to get_item_view
  // or any new invocation frame.
  ItemView &get_item_view(StorageKey const &key);

  AddressAndKey get_current_addrkey(StorageKey const &key) const;

private:
  void revert_frame();
  InvokeResult handle_try_invoke(InvokeResult const &prev_result,
                                 ExecutionModeData &execution_mode_data);

  std::vector<InvocationFrame> frame_stack_;
  std::function<void(InvocationFrame &)> last_frame_action_;
  StorageSnapshotCache &snapshot_cache_;
  PerTransactionDatabaseProxy &db_proxy_;
};

} // namespace hogd
