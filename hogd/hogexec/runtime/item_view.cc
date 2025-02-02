#include "hogd/hogexec/runtime/item_view.h"

#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_item.hh"
#include "syscall.hh"

namespace hogd {

namespace {

StorageDelta
make_bytestring_set(Bytestring const &string)
{
  StorageDelta out(StorageDeltaType::kBytestringSet);
  out.bytestring() = string;
  return out;
}

StorageDelta
make_nnint128_set_add(int128_t set, int128_t add)
{
  StorageDelta out(StorageDeltaType::kNonnegativeIntSetAdd);
  out.nnint() = NonnegativeInt128SetAddArgs{.set_value = to_xdr(set),
                                            .add_value = to_xdr(add)};

  return out;
}

} // namespace

std::optional<StorageItemType>
ItemView::get_item_type() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageItemType> {
        return v.get_item_type();
      },
      delta_args_);
}

std::optional<FlaggedStorageDelta>
ItemView::get_delta() const
{
  return std::visit(
      [](auto const &v) -> std::optional<FlaggedStorageDelta> {
        return v.get_delta();
      },
      delta_args_);
}

FlaggedStorageDelta
BytestringItem::get_delta() const
{
  return FlaggedStorageDelta{.delta = make_bytestring_set(bytestring_),
                             .flags =
                                 lock_exclusive_
                                     ? std::to_underlying(
                                           StorageDeltaModifier::kLockExclusive)
                                     : 0};
}

FlaggedStorageDelta
NNIntItem::get_delta() const
{
  return FlaggedStorageDelta{.delta =
                                 make_nnint128_set_add(base_value_, add_value_),
                             .flags = 0};
}

SyscallStatus<void>
BytestringItem::validate() const
{
  if (bytestring_.size() > kBytestringMaxLen)
    return SyscallError{SyscallErrorCode::kInvalidInput};

  return {};
}

SyscallStatus<void>
NNIntItem::validate() const
{
  if (add_value_ >= 0) {
    // When add_value_ is nonnegative, the storage delta produced
    // by this item is always valid.
    return {};
  }

  if (base_value_ < 0) {
    // cannot subtract from a value that is already negative
    return SyscallError{SyscallErrorCode::kInvalidInput};
  }

  if (base_value_ >= 0) {
    // base_value_ + add_value_ cannot overflow here,
    // because base_value_ and add_value_ have opposite signs
    if (base_value_ + add_value_ < 0) {
      // This would be always rejected by the delta handling logic
      // in the storage server.  Putting this check here
      // may simplify some downstream logic.
      return SyscallError{SyscallErrorCode::kInvalidInput};
    }
  }

  return {};
}

std::optional<StorageItemType>
UnmodifiedItem::get_item_type() const
{
  if (!base_item_) {
    // no base item exists in the snapshot,
    // so object has no type
    return std::nullopt;
  }
  return base_item_->type();
}

SyscallStatus<void>
ItemView::can_accept_type(std::optional<StorageItemType> item_type) const
{
  if (!item_type) {
    return {};
  }

  auto current_type = get_item_type();
  if (!current_type) {
    return {};
  }

  if (*current_type != *item_type) {
    return SyscallError{SyscallErrorCode::kInvalidInput};
  }

  return {};
}

// The logic here should mirror that of advance_cursor()
std::optional<QueueEntry>
RetrocausalQueueItem::get_next_item() const
{
  std::printf("RetrocausalQueueItem::get_next_item()\n");
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

  auto const *next_block = get_next_block(block_index);

  if (next_block) {
    size_t blk_size = next_block->entries.size();
    assert(partial_consume_quantity < blk_size);
    return next_block->entries[partial_consume_quantity];
  }
  // otherwise, nothing in past block to consume.  Try current block.
  assert(partial_consume_quantity == 0);

  // Step 2: first look to consume entries created within the transaction
  if (locally_consumed_index_ < queue_delta_.insert.added_entries.size()) {
    return queue_delta_.insert.added_entries[locally_consumed_index_];
  }

  // Step 3: try to consume concurrent entries not from this transaction
  auto concurrent_entry = get_next_concurrent_entry(locally_consumed_index_);
  if (concurrent_entry) {
    return concurrent_entry;
  }

  // Step 4: nothing to consume
  return std::nullopt;
}

void
RetrocausalQueueItem::insert_entry(QueueEntry const &entry)
{
  queue_delta_.insert.added_entries.push_back(entry);
}

const RetrocausalQueueBlock *
RetrocausalQueueItem::get_next_block(uint64_t search_start_index) const
{
  if (queue_snapshot_cache_entry_) {
    return queue_snapshot_cache_entry_->get_next_block(addrkey_,
                                                       search_start_index);
  }

  return nullptr;
}

std::optional<QueueEntry>
RetrocausalQueueItem::get_next_concurrent_entry(uint32_t index) const
{
  std::printf("RetrocausalQueueItem::get_next_concurrent_entry(%d)\n", index);
  if (queue_snapshot_cache_entry_) {
    return queue_snapshot_cache_entry_->get_concurrent_entry(addrkey_, index);
  }
  return std::nullopt;
}

void
RetrocausalQueueItem::advance_cursor()
{
  std::printf("RetrocausalQueueItem::advance_cursor()\n");
  // Step 1: try consume from past blocks
  // the index where we start searching from

  if (!queue_delta_.consume) {
    queue_delta_.consume.emplace(RetrocausalQueueConsumeDelta{
        .past_block_consume = {.max_fully_consumed_block_number = 0,
                               .partial_consumed_number = 0},
        .current_block_consumed = {}});
  }

  auto const *next_block = get_next_block(
      queue_delta_.consume->past_block_consume.max_fully_consumed_block_number +
      1);

  assert(queue_delta_.consume);

  if (next_block) {
    size_t blk_size = next_block->entries.size();
    assert(queue_delta_.consume->past_block_consume.partial_consumed_number <
           blk_size);
    queue_delta_.consume->past_block_consume.partial_consumed_number++;
    if (queue_delta_.consume->past_block_consume.partial_consumed_number ==
        blk_size) {
      queue_delta_.consume->past_block_consume.max_fully_consumed_block_number =
          next_block->block_number;
      queue_delta_.consume->past_block_consume.partial_consumed_number = 0;
    }
    return;
  }
  // otherwise, nothing in past block to consume.  Try current block.
  assert(queue_delta_.consume->past_block_consume.partial_consumed_number == 0);

  // Step 2: first look to consume entries created within the transaction
  if (locally_consumed_index_ < queue_delta_.insert.added_entries.size()) {
    queue_delta_.consume->current_block_consumed.push_back(
        queue_delta_.insert.added_entries[locally_consumed_index_]);
    locally_consumed_index_++;
    return;
  }

  // Step 3: try to consume concurrent entries not from this transaction
  auto concurrent_entry = get_next_concurrent_entry(locally_consumed_index_);
  if (concurrent_entry) {
    ++locally_consumed_index_;
    queue_delta_.consume->current_block_consumed.push_back(*concurrent_entry);
    return;
  }

  assert(false);
}

} // namespace hogd
