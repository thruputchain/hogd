#include "hogd/hogexec/runtime/invocation_frame.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/mvar.h"
#include "x23/vec.h"

namespace hogd {

InvocationFrame::InvocationFrame(Invocation invocation, InvokeFlagSet flags)
  : active_invocation_(std::move(invocation)), flags_(flags)
{
  if (is_try_invoke()) {
    try_invoke_frame_count_ = 1;
  }
}

void
InvocationFrame::commit_successful_child_frame(InvocationFrame &&frame)
{
  child_return_buffer_ = frame.to_return_;
  logs_.insert(logs_.end(), std::make_move_iterator(frame.logs_.begin()),
               std::make_move_iterator(frame.logs_.end()));

  for (auto const &[addr, hash] : frame.new_contract_deployments_) {
    [[maybe_unused]] auto [_, inserted_new] =
        new_contract_deployments_.emplace(addr, hash);
    assert(inserted_new);
  }

  // transfer modified items from child frame to current frame
  for (auto &[addr, item] : frame.modified_items_) {
    modified_items_.erase(addr);
    modified_items_.emplace(addr, std::move(item));
  }

  try_invoke_frame_count_ += frame.try_invoke_frame_count_;
}

SystemStatus<void>
InvocationFrame::mark_frame_as_failed(
    PerTransactionDatabaseProxy &db_proxy) const
{
  if (try_invoke_frame_count_ > 0) {
    auto st = db_proxy.pop_subtransactions(try_invoke_frame_count_)();
    if (!st.has_value()) {
      return st.error();
    }
  }
  return {};
}

void
InvocationFrame::commit_failed_child_frame()
{
  child_return_buffer_ = std::nullopt;
}

void
InvocationFrame::log(std::span<const std::byte> message)
{
  std::span<const uint8_t> m = {reinterpret_cast<const uint8_t *>(
                                    message.data()),
                                message.size()};
  logs_.emplace_back(active_invocation_.invoked_address,
                     x23::Vec<uint8_t, kMaxLogMessageLen>(m.begin(), m.end()));
}

void
InvocationFrame::set_frame_return(ReturnBuffer &&return_buffer)
{
  to_return_ = std::move(return_buffer);
}

namespace {

x23::Async<std::optional<Nondeterministic<int128_t>>>
do_try_reserve_detached(AddressAndKey const &addrkey, ItemView const &item_view,
                        PerTransactionDatabaseProxy &db_proxy, auto &error,
                        auto &failed)
{
  auto delta = item_view.get_delta();
  if (delta) {
    // Try to reserve the delta in the database.
    auto res = co_await db_proxy.try_reserve_delta(
        addrkey, *delta, std::to_underlying(StorageItemRPCFlags::kStorageNone));

    if (!res.has_value()) {
      // System error: store the first error encountered.
      auto err = error.take();
      if (!err->has_value()) {
        *err = res.error();
      }
      co_return std::nullopt;
    }
    else if (!res.value().has_value()) {
      // Operation failed (but not a system error): mark as failed.
      failed = true;
      co_return std::nullopt;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    co_return res.value().value();
  }
  co_return 0;
}

} // namespace

x23::Async<SystemStatus<bool>>
InvocationFrame::try_acquire_frame_locks(PerTransactionDatabaseProxy &db_proxy,
                                         StorageSnapshotCache &snapshot_cache)
{
  auto [async, cb] = x23::make_async<void>();
  std::atomic<uint32_t> pending_ops = modified_items_.size();
  std::atomic<bool> failed = false;
  x23::MVar<std::optional<SystemError>> error = std::nullopt;

  if (pending_ops == 0) {
    cb();
  }

  // We don't want to worry about modifications to modified_items_ while doing
  // the top-level iteration to create the coroutines.
  x23::MVar<std::map<ContractAddress, Nondeterministic<int128_t>>>
      additional_storage_costs;

  for (auto &[addrkey, item_view] : modified_items_) {
    if (addrkey.key.type() ==
        StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance) {
      // don't pay storage costs for free native token balance entries
      auto count = pending_ops.fetch_sub(1);
      if (count == 1) {
        cb();
      }
      continue;
    }

    [](AddressAndKey const &addrkey, ItemView &item_view, auto &pending_ops,
       auto &cb, PerTransactionDatabaseProxy &db_proxy, auto &error,
       auto &failed, auto &additional_storage_costs) -> x23::Detached {
      co_await db_proxy.coropoll().pool_.bg();

      assert(addrkey.key.type() !=
             StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance);

      auto cost = co_await do_try_reserve_detached(addrkey, item_view, db_proxy,
                                                   error, failed);

      if (cost.has_value()) {
        assert(cost.value().get() >= 0);
        auto map = additional_storage_costs.take();
        // TODO(geoff): Conceivably skippable if cost.value().get() == 0.
        // See note on NativeTokenBalanceItem::get_delta.
        (*map)[addrkey.address].get() += cost.value().get();
      }

      auto count = pending_ops.fetch_sub(1);
      if (count == 1) {
        cb();
      }
    }(addrkey, item_view, pending_ops, cb, db_proxy, error, failed,
                                                     additional_storage_costs);
  }
  // wait for all the operations to finish
  async();

  // check whether some kind of system error occurred
  {
    auto err = error.take();
    if (err->has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      co_return *(*err);
    }
  }

  // check if any of the 2pc database operations failed
  if (failed) {
    co_return !failed;
  }

  // now apply the gathered the storage cost modifications to the items in the
  // current frame
  auto map = additional_storage_costs.take();
  for (auto const &[addr, cost] : *map) {
    auto addrkey = AddressAndKey{
        .address = addr,
        .key =
            StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
    auto view_status = get_or_add_item_view(addrkey, snapshot_cache);

    if (!view_status.has_value()) {
      co_return view_status.error();
    }
    auto *view = view_status.value();
    assert(view);
    auto token_view = [view] {
      if (auto *unmodified_view = view->get<UnmodifiedItem>()) {
        return NativeTokenBalanceItem(
            unmodified_view->base_item_.native_token_balance());
      }
      auto const *token_view = view->get<NativeTokenBalanceItem>();
      assert(token_view);
      return *token_view;
    }();
    token_view.set_nondeterministic_native_token_lock(cost);
    [[maybe_unused]] auto r = view->set(token_view);
    assert(r);
  }

  // now try to reserve the storage costs in the database

  auto [async_storage, cb_storage] = x23::make_async<void>();
  pending_ops = modified_items_.size();
  if (pending_ops == 0) {
    cb_storage();
  }

  for (auto const &[addrkey, item_view] : modified_items_) {
    if (addrkey.key.type() !=
        StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance) {
      // now we skip all the non-storage-cost entries
      auto count = pending_ops.fetch_sub(1);
      if (count == 1) {
        cb_storage();
      }
      continue;
    }

    [](AddressAndKey const &addrkey, ItemView const &item_view,
       auto &pending_ops, auto &cb, PerTransactionDatabaseProxy &db_proxy,
       auto &error, auto &failed) -> x23::Detached {
      co_await db_proxy.coropoll().pool_.bg();

      assert(addrkey.key.type() ==
             StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance);

      // storage cost is not paid for these entries; so we just check if the
      // operation failed, which is handled inside do_try_reserve_detached
      auto _ = co_await do_try_reserve_detached(addrkey, item_view, db_proxy,
                                                error, failed);

      auto count = pending_ops.fetch_sub(1);
      if (count == 1) {
        cb();
      }
    }(addrkey, item_view, pending_ops, cb_storage, db_proxy, error, failed);
  }

  async_storage();
  // check whether some kind of system error occurred
  {
    auto err = error.take();
    if (err->has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      co_return *(*err);
    }
  }

  // check if any of the storage reservation operations failed
  co_return !failed;
}

SystemStatus<void>
InvocationFrameStack::start_frame(Invocation const &invocation,
                                  InvokeFlagSet flags)
{
  frame_stack_.emplace_back(invocation, flags);
  if (frame_stack_.back().is_try_invoke()) {
    auto st = db_proxy_.push_new_subtransaction()();
    if (!st.has_value()) {
      return st.error();
    }
  }
  return {};
}

SystemStatus<InvokeResult>
InvocationFrameStack::finish_frame(InvokeResult result,
                                   ExecutionModeData &execution_mode_data)
{
  // handle the try_invoke data
  assert(!frame_stack_.empty());
  if (frame_stack_.back().is_try_invoke()) {
    // Only relevant in the case where the deterministic part of the frame
    // succeeds.
    if (result.is_frame_success()) {
      bool acq_success = false;

      // If we're in proposal mode, we need to acquire the frame locks.
      if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
        auto acq_res =
            frame_stack_.back().try_acquire_frame_locks(db_proxy_,
                                                        snapshot_cache_)();
        if (!acq_res.has_value()) {
          return acq_res.error();
        }
        acq_success = acq_res.value();

        std::get<ProposalModeData>(execution_mode_data)
            .log_nondeterministic_bit(acq_success);
      }
      else {
        // Otherwise, we're in validation mode, so we read the nondeterministic
        // bit from the validation metadata.
        auto res = std::get<ValidationModeData>(execution_mode_data)
                       .consume_nondeterministic_bit();
        if (!res.has_value()) {
          return SystemError{SystemErrorCode::kValidationFailure};
        }
        acq_success = res.value().get();
      }

      // If the frame locks were not acquired successfully, then we mark
      // the entire frame as failing.
      if (!acq_success) {
        // TODO(geoff): perhaps add additional gas consumption
        result = InvokeResult{
            .result_ = InvokeError{InvokeErrorCode::kInvokeDeterministic},
            .gas_consumed_ = result.gas_consumed_,
        };
      }
    }
    // otherwise nothing to do here with try_invoke
  }

  // If a contract has the REVERT_SAFE flag set, then it can be called again
  // after it has been reverted. A contract might not be REVERT_SAFE because
  // reversion does NOT reset a wasm's internal memory or other internal state.
  // We could do that, it's just expensive,
  // and would require fairly close integration with the wasm runtime and some
  // fairly invasive changes (We need to restore the entirety of the wasm
  // module's internal state -- not just memory, but also tables and globals.
  // The wasm stack is reset automatically).
  if (result.should_frame_revert()) {
    auto st = frame_stack_.back().mark_frame_as_failed(db_proxy_);
    if (!st.has_value()) {
      return st.error();
    }
    frame_stack_.pop_back();

    if (!frame_stack_.empty()) {
      frame_stack_.back().commit_failed_child_frame();
    }

    return result;
  }

  // commit child invocation frame successfully
  assert(result.is_frame_success());

  size_t stack_size = frame_stack_.size();
  if (stack_size >= 2) {
    // Commit the frame to it's parent
    frame_stack_[stack_size - 2].commit_successful_child_frame(
        std::move(frame_stack_.back()));
  }
  else if (stack_size == 1) {
    // last frame -- push frame contents to external caller
    // (SystemCallHandler), via the supplied lambda
    last_frame_action_(std::move(frame_stack_.back()));
  }

  frame_stack_.pop_back();

  return result;
}

std::optional<ContractAddress>
InvocationFrameStack::get_msg_sender() const
{
  return frame_stack_.size() >= 2
             ? std::optional<ContractAddress>{frame_stack_[frame_stack_.size() -
                                                           2]
                                                  .active_invocation()
                                                  .invoked_address}
             : std::nullopt;
}

std::optional<SHA256Hash>
InvocationFrameStack::lookup_locally_deployed_contract(
    ContractAddress const &addr) const
{
  for (auto const &frame : frame_stack_) {
    auto it = frame.new_contract_deployments_.find(addr);
    if (it != frame.new_contract_deployments_.end()) {
      // contract was deployed at `addr` during a frame earlier on the frame
      // stack. Thus, deployment exists during the current view.
      return it->second;
    }
  }
  return std::nullopt;
}

ItemView *
InvocationFrame::get_item_view(AddressAndKey const &addrkey)
{
  auto it = modified_items_.find(addrkey);
  if (it != modified_items_.end()) {
    return &(it->second);
  }
  return nullptr;
}

ItemView *
InvocationFrame::add_item_view(AddressAndKey const &addrkey, ItemView new_view)
{
  assert(!modified_items_.contains(addrkey));
  auto [iter, _] = modified_items_.emplace(addrkey, std::move(new_view));
  return &iter->second;
}

SystemStatus<ItemView *>
InvocationFrame::get_or_add_item_view(AddressAndKey const &addrkey,
                                      StorageSnapshotCache &snapshot_cache)
{
  if (auto *view = get_item_view(addrkey)) {
    return view;
  }

  auto snapshot_item = snapshot_cache.try_get_snapshot_item(addrkey);
  if (!snapshot_item.has_value()) {
    return snapshot_item.error();
  }
  assert(snapshot_item.value());
  return add_item_view(addrkey, ItemView(snapshot_item.value()));
}

SystemStatus<ItemView *>
InvocationFrameStack::get_item_view(StorageKeyContractData const &data_key)
{
  const int32_t stack_depth = frame_stack_.size();
  assert(stack_depth > 0);

  StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
  key.contract_data_key() = data_key;

  auto &active_frame = get_active_frame();
  AddressAndKey addrkey{.address =
                            active_frame.active_invocation().invoked_address,
                        .key = key};

  // do we already have a frame in the top active frame
  if (auto *active_frame_view = active_frame.get_item_view(addrkey)) {
    return active_frame_view;
  }

  // otherwise create view in active frame, by either copying from
  // a parent call frame or from storage
  for (int32_t i = stack_depth - 2; i >= 0; --i) {
    auto &frame = frame_stack_[i];

    if (auto const *local_view = frame.get_item_view(addrkey)) {
      // refresh the parent frame view in the top frame
      return active_frame.add_item_view(addrkey, *local_view);
    }
  }

  // item view not yet accessed in current transaction; looking up in snapshot
  auto snapshot_item = snapshot_cache_.try_get_snapshot_item(addrkey);
  if (!snapshot_item) {
    return snapshot_item.error();
  }

  // snapshot_item might be nullptr; this is handled in ItemView
  return frame_stack_.back().add_item_view(addrkey,
                                           ItemView(snapshot_item.value()));
}

} // namespace hogd
