#include "hogd/hogexec/runtime/item_view.h"

#include <cassert>
#include <optional>
#include <variant>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall.hh"

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
make_nnint128_set_add(int128_t set, int128_t add)
{
  StorageDelta out(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
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
        if constexpr (requires { v.kItemType; }) {
          return v.kItemType;
        }
        else {
          return v.get_item_type();
        }
      },
      delta_args_);
}

std::optional<StorageDelta>
ItemView::get_delta() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageDelta> {
        return v.get_delta();
      },
      delta_args_);
}

SyscallStatus<void>
ItemView::can_accept_type(std::optional<StorageItemType> item_type) const
{
  if (!item_type) {
    // Writing a deletion; this is always allowed
    return {};
  }

  auto current_type = get_item_type();
  if (!current_type) {
    // Item is in deleted state; therefore it is allowed to write anything
    return {};
  }

  if (*current_type != *item_type) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  return {};
}

StorageDelta
BytestringItem::get_delta() const
{
  return make_bytestring_set(bytestring_);
}

StorageDelta
NNIntItem::get_delta() const
{
  return make_nnint128_set_add(base_value_, add_value_);
}

SyscallStatus<void>
BytestringItem::validate() const
{
  if (bytestring_.size() > kBytestringMaxLen)
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};

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
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  if (base_value_ >= 0) {
    // base_value_ + add_value_ cannot overflow here,
    // because base_value_ and add_value_ have opposite signs
    if (base_value_ + add_value_ < 0) {
      // This would be always rejected by the delta handling logic
      // in the storage server.  Putting this check here
      // may simplify some downstream logic.
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
  }

  return {};
}

// RetrocausalQueueItem implementation

// The logic here should mirror that of advance_cursor()
HostFnStatus<std::optional<QueueEntry>>
RetrocausalQueueItem::get_next_item(ExecutionModeData &execution_mode_data,
                                    bool advance_cursor)
{
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

  auto next_block_status = get_next_block(block_index);
  if (!next_block_status.has_value()) {
    return next_block_status.error().to_host_fn_error();
  }
  auto const *next_block = next_block_status.value();

  if (next_block) {
    assert(next_block->entries.size() > 0);
    assert(partial_consume_quantity < next_block->entries.size());

    if (advance_cursor) {
      size_t blk_size = next_block->entries.size();
      assert(!queue_delta_.consume ||
             queue_delta_.consume->past_block_consume.partial_consumed_number <
                 blk_size);
      if (!queue_delta_.consume) {
        assert(next_block->block_number > 0);
        queue_delta_.consume.emplace(RetrocausalQueueConsumeDelta{
            .past_block_consume =
                {
                    .max_fully_consumed_block_number =
                        0, // can be 0, if we don't consume a full block
                    .partial_consumed_number = 0,
                },
            .current_block_consumed = {},
        });
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
    }

    return next_block->entries[partial_consume_quantity];
  }
  // otherwise, nothing in past block to consume.  Try current block.
  assert(partial_consume_quantity == 0);

  // Step 2: look to consume entries created within the transaction
  auto const *self_consume_res = get_next_self_entry(self_consumed_index_);

  if (self_consume_res) {
    if (advance_cursor) {
      ++self_consumed_index_;
      queue_delta_.consume->current_block_consumed.push_back(*self_consume_res);
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
      queue_delta_.consume->current_block_consumed.push_back(
          *concurrent_entry.value());
    }
    // NOLINTEND(bugprone-unchecked-optional-access)

    return {concurrent_entry};
  }

  // Step 4: nothing to consume
  return std::nullopt;
}

void
RetrocausalQueueItem::insert_entry(QueueEntry const &entry)
{
  queue_delta_.insert.added_entries.push_back(entry);
}

SystemStatus<const RetrocausalQueueBlock *>
RetrocausalQueueItem::get_next_block(uint64_t search_start_index) const
{
  if (base_item_.type() == StorageItemType::kItemTypeRetrocausalQueue) {
    return base_item_.get_next_rcq_block(search_start_index);
  }
  return nullptr;
}

HostFnStatus<std::optional<QueueEntry>>
RetrocausalQueueItem::get_next_concurrent_entry(
    uint32_t index, ExecutionModeData &execution_mode_data) const
{
  if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
    auto out = base_item_.get_concurrent_rcq_entry(index);
    if (!out.has_value()) {
      return out.error().to_host_fn_error();
    }

    auto &proposal_data = std::get<ProposalModeData>(execution_mode_data);
    auto &rcq_entry = out.value();

    if (rcq_entry.get().has_value()) {
      proposal_data.log_queue_entry_observation(base_item_.addrkey(), index,
                                                {*rcq_entry.get()});
      proposal_data.log_nondeterministic_bit({true});
    }
    else {
      proposal_data.log_nondeterministic_bit({false});
    }

    // OK to return not tagged as nondeterministic: the nondeterminism has been
    // logged above
    return rcq_entry.get();
  }
  // else validation mode
  assert(std::holds_alternative<ValidationModeData>(execution_mode_data));
  auto &validation_data = std::get<ValidationModeData>(execution_mode_data);
  auto bit_res = validation_data.consume_nondeterministic_bit();
  if (!bit_res.has_value()) {
    return HostFnError{HostFnErrorCode::kHostValidationFailure};
  }
  if (bit_res->get()) {
    auto res =
        validation_data.consume_queue_entry_observation(base_item_.addrkey(),
                                                        index);
    if (!res.has_value()) {
      return HostFnError{HostFnErrorCode::kHostValidationFailure};
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

std::optional<StorageItemType>
UnmodifiedItem::get_item_type() const
{
  if (base_item_.type() == StorageItemType::kItemTypeDeleted) {
    // no base object exists in the snapshot,
    // so object has no type
    return std::nullopt;
  }
  return base_item_.type();
}

} // namespace hogd
