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
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/status.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class PerTransactionDatabaseProxy;
struct InvokeResult;

// Stores all relevant data structures for one Invocation in a transaction.
class InvocationFrame {
public:
  InvocationFrame(ContractAddress const &address, Calldata &&calldata,
                  InvokeFlagSet flags);

  // Disable copy construction/assignment but allow move ctor
  InvocationFrame(InvocationFrame const &) = delete;
  InvocationFrame &operator=(InvocationFrame const &) = delete;

  // NOLINTNEXTLINE(bugprone-exception-escape)
  InvocationFrame(InvocationFrame &&) noexcept = default;
  InvocationFrame &operator=(InvocationFrame &&) noexcept = delete;

  ContractAddress const &address() const { return address_; }
  Calldata const &calldata() const { return calldata_; }

  // new deployments that were created during this (and child) call frames
  std::map<ContractAddress, SHA256Hash> new_contract_deployments_;

  // The value of the last _child_ call frame's returned string.
  // Set to nullopt on failed calls, or before any child call happens.
  std::optional<ReturnBuffer> child_return_buffer_ = std::nullopt;

  // merge child invocation frame into self
  void commit_successful_child_frame(InvocationFrame &&frame);

  // revert the child invocation frame.
  // Does not clean up the child frame, but is responsible for handling the
  // parent frame's response to the child frame's failure.
  void commit_failed_child_frame();

  // Mark the current frame as failed.
  // Used to clean up failed try_invoke frames.
  SystemStatus<void>
  mark_frame_as_failed(PerTransactionDatabaseProxy &db_proxy) const;

  // set the value returned from the current call frame.
  void set_frame_return(ReturnBuffer &&return_buffer);

  // add a log message to the output of the transaction, associated with the
  // current call frame.
  void log(std::span<const std::byte> message);
  std::vector<LogEntry> extract_logs() { return std::move(logs_); }

  bool is_try_invoke() const
  {
    return (flags_ & std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke)) != 0;
  }

  bool is_read_only() const
  {
    return (flags_ & std::to_underlying(InvokeFlag::kInvokeFlagReadOnly)) != 0;
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

  SystemStatus<bool>
  try_acquire_frame_locks(PerTransactionDatabaseProxy &db_proxy);

private:
  const ContractAddress address_;
  const Calldata calldata_;

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
      PerTransactionDatabaseProxy &db_proxy)
    : last_frame_action_(std::move(last_frame_action)), db_proxy_(db_proxy)
  {}

  auto &get_active_frame(this auto &self)
  {
    assert(!self.frame_stack_.empty());
    return self.frame_stack_.back();
  }
  bool empty() const { return frame_stack_.empty(); }

  void start_frame(ContractAddress const &address, Calldata &&calldata,
                   InvokeFlagSet flags);
  SystemStatus<InvokeResult>
  finish_frame(InvokeStatus<void> result, ExecutionModeData &execution_mode_data);

  std::optional<ContractAddress> get_msg_sender() const;

  // Lookup the hash of the contract deployed at the queried address,
  // _only_ for contracts that are deployed during the current transaction.
  // Allows deployments to be created and then invoked during one transaction
  // (useful for initialization).
  //
  // Also usable to check whether a deployment was rolled back by a reverted
  // call frame.
  std::optional<SHA256Hash>
  lookup_locally_deployed_contract(ContractAddress const &addr) const;

  // Reference invalidated by any other call to get_item_view
  // The returned pointer is never null
  ItemView *get_item_view(StorageKeyContractData const &data_key);
  ItemView *get_item_view(AddressAndKey const &addrkey);

  AddressAndKey get_addrkey(StorageKeyContractData const &data_key) const
  {
    StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
    key.contract_data_key() = data_key;
    return AddressAndKey{.address = get_active_frame().address(), .key = key};
  }

private:
  std::vector<InvocationFrame> frame_stack_;
  std::function<void(InvocationFrame &&)> last_frame_action_;
  PerTransactionDatabaseProxy &db_proxy_;
};

} // namespace hogd
