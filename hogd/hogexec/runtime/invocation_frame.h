#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contract.hh"
#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/components/hash_buffers.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

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

  // The value of the last _child_ call frame's return value.
  // Set to nullopt before any child call happens.
  std::optional<InvocationFrameOutput> child_return_output_ = std::nullopt;

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
  void set_frame_return(InvocationFrameOutput &&return_output);

  // add a log message to the output of the transaction, associated with the
  // current call frame.
  void log_event(TxEmittedEvent event);

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

  std::unordered_set<SHA256Hash> get_committed_contracts() const
  {
    std::unordered_set<SHA256Hash> committed_contracts;
    committed_contracts.reserve(new_contract_deployments_.size());
    for (auto const &[addr, deployment] : new_contract_deployments_) {
      committed_contracts.insert(deployment.hash);
    }
    return committed_contracts;
  }

  HashBuffers hash_buffers_;

private:
  const ContractAddress address_;
  const Calldata calldata_;

  // buffer to return to parent
  InvocationFrameOutput to_return_output_;

  // flags on the invocation
  const InvokeFlagSet flags_;

  // Accumulates the events created in the current call frame (and child frames)
  x23::Vec<TxEmittedEvent> events_;

  uint32_t try_invoke_frame_count_ = 0;

  // elements modified during the current call frame (and child frames)
  std::unordered_map<AddressAndKey, ItemView> modified_items_;

  // newly deployed contracts during the current call frame (and child frames)
  std::unordered_map<ContractAddress, DeployedContractHash>
      new_contract_deployments_;
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
  size_t event_count() const;

  void start_frame(ContractAddress const &address, Calldata &&calldata,
                   InvokeFlagSet flags);
  // Returns a pair of the InvokeResult and a bool indicating whether the frame
  // was manually reverted.
  SystemStatus<std::pair<InvokeResult, bool>>
  finish_frame(InvokeStatus<void> result,
               ExecutionModeData &execution_mode_data);

  std::optional<ContractAddress> get_msg_sender() const;

  bool has_active_frame_for(ContractAddress const &addr) const
  {
    return std::ranges::any_of(frame_stack_, [&](const InvocationFrame &frame) {
      return frame.address() == addr;
    });
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
    return AddressAndKey{.address = get_active_frame().address(), .key = key};
  }

  size_t size() const { return frame_stack_.size(); }

private:
  std::vector<InvocationFrame> frame_stack_;
  std::function<void(InvocationFrame &&)> last_frame_action_;
  PerTransactionDatabaseProxy &db_proxy_;
};

} // namespace hogd
