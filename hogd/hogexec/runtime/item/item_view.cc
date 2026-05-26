#include "hogd/hogexec/runtime/item/item_view.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <variant>

#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/item/item_view_type.h"
#include "hogd/hogexec/runtime/item/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

StorageDelta
make_bytestring_set(Bytestring const &string)
{
  StorageDelta out(StorageDeltaType::kDeltaTypeBytestringSet);
  out.bytestring() = string;
  return out;
}

StorageDelta
make_nnint128_set_add(int128_xdr set, int128_xdr add)
{
  StorageDelta out(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
  out.nnint() = NonnegativeInt128SetAddArgs{.set_value = set, .add_value = add};

  return out;
}

StorageDelta
make_nnint128_add(int128_xdr add)
{
  StorageDelta out(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
  out.nnint_add() = add;
  return out;
}

} // namespace

std::pair<std::optional<StorageDelta>, StorageDeltaFlagSet>
ItemView::get_delta() const
{
  return {std::visit(
              [](auto const &v) -> std::optional<StorageDelta> {
                return v.get_delta();
              },
              delta_args_),
          flags_};
}

StorageDelta
BytestringItem::get_delta() const
{
  return make_bytestring_set(bytestring_);
}

StorageDelta
NNIntItem::get_delta() const
{
  if (base_value_.has_value()) {
    return make_nnint128_set_add(base_value_.value(), add_value_);
  }
  return make_nnint128_add(add_value_);
}

SyscallStatus<void>
BytestringItem::validate() const
{
  // The bytestring is coming from untrusted contract input.
  // We should not get to this point with a bytestring that is too long.
  // That might be a security risk in extreme cases --- a contract could ask us
  // to allocate a Bytestring that is extremely long (as long as its linear
  // memory).  Given current linear memory limits, it would not be possible to
  // allocate a bytestring long enough to exhaust memory resources and/or cause
  // malloc to fail, but a contract could still adversarially waste system
  // resources.
  CHECK_LE(bytestring_.size(), static_cast<size_t>(kBytestringMaxLen));

  return {};
}

SyscallStatus<void>
NNIntItem::validate() const
{
  int128_t add_value = from_xdr(add_value_);
  if (add_value >= 0) {
    // A nonnegative add_value is always valid, even with a negative
    // set_value (base).  The protocol invariant is
    // value >= min(set_value, 0), NOT value >= 0.  Negative set_values
    // intentionally commit negative NNInt values; the lock system
    // enforces the lower bound accordingly.  See #2155.
    return {};
  }
  if (!base_value_.has_value()) {
    return {};
  }

  int128_t base_value = from_xdr(*base_value_);

  if (base_value < 0) {
    // cannot subtract from a value that is already negative
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  if (base_value >= 0) {
    // base_value_ + add_value_ cannot overflow here,
    // because base_value_ and add_value_ have opposite signs
    if (base_value + add_value < 0) {
      // This would be always rejected by the delta handling logic
      // in the storage server.
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
  }

  return {};
}

// RetrocausalQueueItem implementation

RetrocausalQueueItem
RetrocausalQueueItem::make_fully_consumed_item(
    const SnapshotStorageItem &base_item, x23::Vec<QueueEntry> consumed_entries)
{
  auto retrocausally_consumed_count =
      static_cast<uint32_t>(consumed_entries.size());

  RetrocausalQueueItem item(base_item);
  item.locally_consumed_index_ = retrocausally_consumed_count;

  bool need_consume_delta =
      retrocausally_consumed_count > 0 ||
      (base_item.type() == ItemViewType::kRetrocausalQueue &&
       base_item.retrocausal_queue_metadata().live_entries > 0);

  if (need_consume_delta) {
    RetrocausalQueuePastBlockConsumeDelta past_consume = {
        .max_fully_consumed_block_number = 0,
        .partial_consumed_number = 0,
    };
    if (base_item.type() == ItemViewType::kRetrocausalQueue) {
      auto const &meta = base_item.retrocausal_queue_metadata();
      past_consume.max_fully_consumed_block_number = meta.max_live_block_num;
    }
    item.queue_delta_.consume.emplace(RetrocausalQueueConsumeDelta{
        .past_block_consume = past_consume,
        .current_block_consumed = std::move(consumed_entries),
    });
  }

  return item;
}

void
RetrocausalQueueItem::construct_consume_delta()
{
  assert(base_item_);
  RetrocausalQueuePastBlockConsumeDelta past_consume = {
      .max_fully_consumed_block_number = 0,
      .partial_consumed_number = 0,
  };

  if (base_item_->type() == ItemViewType::kRetrocausalQueue) {
    past_consume = base_item_->retrocausal_queue_metadata().consume_state;
  }

  queue_delta_.consume.emplace(RetrocausalQueueConsumeDelta{
      .past_block_consume = past_consume,
      .current_block_consumed = {},
  });
}

// This queue consumes entries in the following order:
// 1. Entries from past blocks.
// 2. self entries created within the transaction
// 3. concurrent entries not from this transaction
//
// 2 and 3 can get interleaved if, for example,
// a transaction first consumes a concurrent entry and then
// creates a self entry.
//
// As a subtle point, this ordering is important for providing
// a sensible "peek" behavior.  If I peek at the head of the queue,
// then later, when I consume the next entry, I should logically always
// get that same entry.  But if we swapped steps 2 and 3, then
// the entry I peeked at might not be the same as the entry I consume
// (since a concurrent entry might have been added in the meantime).
//
// You could make a similar argument about peeking at the head of the queue
// and seeing a concurrent entry, and then doing an insert and now peek returns
// that self entry.  This is less weird of an ordering, because the observed
// behavior of peek changed in direct response to an action by the contract.
SystemStatus<std::optional<QueueEntry>>
RetrocausalQueueItem::get_next_item(ExecutionModeData &execution_mode_data,
                                    bool advance_cursor)
{
  assert(base_item_);
  // Step 1: try consume from past blocks
  // the index where we start searching from
  uint64_t block_index = 0;
  uint64_t partial_consume_quantity = 0;
  if (queue_delta_.consume) {
    block_index = queue_delta_.consume->past_block_consume
                      .max_fully_consumed_block_number +
                  1;
    partial_consume_quantity =
        queue_delta_.consume->past_block_consume.partial_consumed_number;
  }
  else {
    // If we don't have a consume delta yet, then we're starting from
    // wherever things left off according to the snapshot.
    if (base_item_->type() == ItemViewType::kRetrocausalQueue) {
      auto const &meta = base_item_->retrocausal_queue_metadata();
      block_index = meta.consume_state.max_fully_consumed_block_number + 1;
      partial_consume_quantity = meta.consume_state.partial_consumed_number;
    }
    else {
      block_index = 1;
      partial_consume_quantity = 0;
    }
  }

  auto next_block_status = get_next_block(block_index);
  if (!next_block_status.has_value()) {
    return next_block_status.error();
  }
  auto const *next_block = next_block_status.value();

  if (next_block) {
    assert(!next_block->entries.empty());
    assert(partial_consume_quantity < next_block->entries.size());

    if (advance_cursor) {
      size_t blk_size = next_block->entries.size();
      assert(!queue_delta_.consume ||
             queue_delta_.consume->past_block_consume.partial_consumed_number <
                 blk_size);
      if (!queue_delta_.consume) {
        assert(next_block->block_number > 0);
        construct_consume_delta();
      }
      queue_delta_.consume->past_block_consume.partial_consumed_number++;

      if (queue_delta_.consume->past_block_consume.partial_consumed_number ==
          blk_size) {
        // We consumed the entirety of next_block -- so advance to mark
        // next_block as fully consumed and reset the partial_consumed_number
        // to 0.
        queue_delta_.consume->past_block_consume
            .max_fully_consumed_block_number = next_block->block_number;
        queue_delta_.consume->past_block_consume.partial_consumed_number = 0;
      }
      past_block_consumed_count_++;
    }

    return next_block->entries[partial_consume_quantity];
  }

  // otherwise, nothing in past block to consume.  Try current block.
  assert(partial_consume_quantity == 0);

  // Step 2: look to consume entries created within the transaction
  auto const *self_consume_res = get_next_self_entry(self_consumed_index_);

  if (self_consume_res) {
    if (advance_cursor) {
      CHECK_LT(self_consumed_index_,
               static_cast<uint16_t>(kRetrocausalQueueBlockEntriesMaxLen));
      ++self_consumed_index_;
      if (!queue_delta_.consume) {
        construct_consume_delta();
      }
    }
    return {*self_consume_res};
  }

  // Step 3: try to consume concurrent entries not from this transaction
  auto concurrent_entry =
      get_next_concurrent_entry(locally_consumed_index_, execution_mode_data);
  if (!concurrent_entry.has_value()) {
    return concurrent_entry.error();
  }

  if (concurrent_entry->has_value()) {

    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    if (advance_cursor) {
      ++locally_consumed_index_;
      if (!queue_delta_.consume) {
        construct_consume_delta();
      }
      if (queue_delta_.consume->current_block_consumed.size() >=
          kRetrocausalQueueBlockEntriesMaxLen) {
        CHECK(!std::holds_alternative<ProposalModeData>(execution_mode_data));
        return SystemError{SystemErrorCode::ValidationFailure};
      }
      queue_delta_.consume->current_block_consumed.push_back(
          *concurrent_entry.value());
    }
    // NOLINTEND(bugprone-unchecked-optional-access)

    return {concurrent_entry};
  }

  // Step 4: nothing to consume
  return std::nullopt;
}

StorageDelta
RetrocausalQueueItem::get_delta() const
{
  StorageDelta delta =
      StorageDelta(StorageDeltaType::kDeltaTypeRetrocausalQueue);

  // load the retrocausally consumed entries
  if (queue_delta_.consume) {
    // TODO(geoff): In proposal mode, we could reduce some copying in the
    // ItemView by loading this list of consumed entries from the snapshot base
    // item.
    delta.retrocausal_queue().consume = queue_delta_.consume;
  }
  CHECK_LE(self_consumed_index_, queue_delta_.insert.added_entries.size());
  delta.retrocausal_queue().insert.added_entries.append(
      queue_delta_.insert.added_entries.begin() + self_consumed_index_,
      queue_delta_.insert.added_entries.end());

  // In debug builds, we shuffle the order of entries in the concurrent
  // consumes, as these are not supposed to affect the result of the db
  // operation.
  if constexpr (is_debug_build()) {
    if (delta.retrocausal_queue().consume) {
      std::shuffle(
          delta.retrocausal_queue().consume->current_block_consumed.begin(),
          delta.retrocausal_queue().consume->current_block_consumed.end(),
          std::mt19937{std::random_device{}()});
    }
    // We don't shuffle the order of this-block inserts -- some of the tests
    // have logic conditioned on the order of concurrent inserts.
  }
  return delta;
}

SyscallStatus<void>
RetrocausalQueueItem::insert_entry(QueueEntry const &entry)
{
  if (queue_delta_.insert.added_entries.size() >=
      kRetrocausalQueueBlockEntriesMaxLen) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  queue_delta_.insert.added_entries.push_back(entry);
  return {};
}

SystemStatus<const RetrocausalQueueBlock *>
RetrocausalQueueItem::get_next_block(uint64_t search_start_index) const
{
  assert(base_item_);
  if (base_item_->type() == ItemViewType::kRetrocausalQueue) {
    return base_item_->get_next_rcq_block(search_start_index);
  }
  return nullptr;
}

SystemStatus<std::optional<QueueEntry>>
RetrocausalQueueItem::get_next_concurrent_entry(
    uint32_t index, ExecutionModeData &execution_mode_data) const
{
  assert(base_item_);
  if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
    auto out = base_item_->get_concurrent_rcq_entry(index);
    if (!out.has_value()) {
      return out.error();
    }

    auto &proposal_data = std::get<ProposalModeData>(execution_mode_data);
    auto &rcq_entry = out.value();

    if (rcq_entry.get().has_value()) {
      // NOLINTBEGIN(bugprone-unchecked-optional-access)
      proposal_data.log_queue_entry_observation(base_item_->addrkey(), index,
                                                {*rcq_entry.get()});
      // NOLINTEND(bugprone-unchecked-optional-access)

      proposal_data.log_nondeterministic_bit({true});
    }
    else {
      proposal_data.log_nondeterministic_bit({false});
    }

    // OK to return not tagged as nondeterministic: the nondeterminism has been
    // logged above
    return rcq_entry.get();
  }
  if (std::holds_alternative<ViewingModeData>(execution_mode_data)) {
    auto &viewing_data = std::get<ViewingModeData>(execution_mode_data);
    auto bit = viewing_data.view_query_queue_observation_bit();
    if (!bit.get()) {
      // no queue entry available
      return {std::nullopt};
    }
    // queue entry available
    auto entry =
        viewing_data.view_queue_entry_observation(base_item_->addrkey(), index);
    if (!entry.has_value()) {
      return SystemError{SystemErrorCode::ValidationFailure};
    }
    return entry.value().get();
  }
  // else validation mode
  assert(std::holds_alternative<ValidationModeData>(execution_mode_data));
  auto &validation_data = std::get<ValidationModeData>(execution_mode_data);
  auto bit_res = validation_data.consume_nondeterministic_bit();
  if (!bit_res.has_value()) {
    return SystemError{SystemErrorCode::ValidationFailure};
  }
  if (bit_res->get()) {
    auto res =
        validation_data.consume_queue_entry_observation(base_item_->addrkey(),
                                                        index);
    if (!res.has_value()) {
      return SystemError{SystemErrorCode::ValidationFailure};
    }
    return res.value().get();
  }
  return {std::nullopt};
}

const QueueEntry *
RetrocausalQueueItem::get_next_self_entry(uint32_t index) const
{
  if (index < queue_delta_.insert.added_entries.size()) {
    return &queue_delta_.insert.added_entries[index];
  }
  return nullptr;
}

StorageDelta
ReplayCacheItem::get_delta() const
{
  auto delta = StorageDelta(StorageDeltaType::kDeltaTypeConsumeUniquifier);
  delta.consume_uniquifier_args() =
      ConsumeUniquifierArgs{.uniquifier = consumed_uniquifier_};
  return delta;
}

StorageDelta
AddressAuthDataItem::get_delta() const
{
  auto delta = StorageDelta(StorageDeltaType::kDeltaTypeAuthDataChange);
  assert(internal_auth_data_);
  for (auto const &[key_index, new_key] : internal_auth_data_->pk_changes_) {
    delta.auth_data_change().auth_data_change_signers.push_back(
        AuthDataChangeSigner{.public_key_id = key_index,
                             .public_key = new_key});
  }
  if (internal_auth_data_->required_weight_.has_value()) {
    delta.auth_data_change().new_required_weight =
        internal_auth_data_->required_weight_.value();
  }
  // In debug builds, we shuffle the order of public key changes,
  // because the order of pk changes in the delta is not supposed to affect the
  // result of the delta handling logic.
  if constexpr (is_debug_build()) {
    std::shuffle(delta.auth_data_change().auth_data_change_signers.begin(),
                 delta.auth_data_change().auth_data_change_signers.end(),
                 std::mt19937{std::random_device{}()});
  }
  return delta;
}

AddressAuthDataItem &
AddressAuthDataItem::operator=(AddressAuthDataItem const &other)
{
  if (this == &other) {
    return *this;
  }
  internal_auth_data_ =
      std::make_unique<InternalAuthData>(*other.internal_auth_data_);
  return *this;
}

std::optional<ItemViewType>
UnmodifiedItem::get_item_type() const
{
  assert(base_item_.has_value());
  assert(base_item_.value());
  return base_item_ // NOLINT(bugprone-unchecked-optional-access)
      .value()
      ->type();
}

SystemStatus<void>
UnmodifiedItem::load(StorageSnapshotCache &snapshot_cache,
                     AddressAndKey const &addrkey) const
{
  auto r = snapshot_cache.try_get_snapshot_item(addrkey);
  if (!r.has_value()) {
    return r.error();
  }
  base_item_ = r.value();
  return {};
}

} // namespace hogd
