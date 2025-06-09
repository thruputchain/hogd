#include "hogd/hogexec/runtime/item_view.h"

#include <cassert>
#include <optional>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view_type.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "types.hh"

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

std::optional<StorageDelta>
ItemView::get_delta() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageDelta> {
        return v.get_delta();
      },
      delta_args_);
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

void
RetrocausalQueueItem::add_empty_consume_delta()
{
  queue_delta_.consume.emplace(RetrocausalQueueConsumeDelta{
      .past_block_consume =
          {
              .max_fully_consumed_block_number = 0,
              .partial_consumed_number = 0,
          },
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
SystemStatus<std::optional<QueueEntry>>
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
        add_empty_consume_delta();
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
      if (!queue_delta_.consume) {
        add_empty_consume_delta();
      }
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
      if (!queue_delta_.consume) {
        add_empty_consume_delta();
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

void
RetrocausalQueueItem::insert_entry(QueueEntry const &entry)
{
  queue_delta_.insert.added_entries.push_back(entry);
}

SystemStatus<const RetrocausalQueueBlock *>
RetrocausalQueueItem::get_next_block(uint64_t search_start_index) const
{
  if (base_item_.type() == ItemViewType::kRetrocausalQueue) {
    return base_item_.get_next_rcq_block(search_start_index);
  }
  return nullptr;
}

SystemStatus<std::optional<QueueEntry>>
RetrocausalQueueItem::get_next_concurrent_entry(
    uint32_t index, ExecutionModeData &execution_mode_data) const
{
  if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
    auto out = base_item_.get_concurrent_rcq_entry(index);
    if (!out.has_value()) {
      return out.error();
    }

    auto &proposal_data = std::get<ProposalModeData>(execution_mode_data);
    auto &rcq_entry = out.value();

    if (rcq_entry.get().has_value()) {
      // NOLINTBEGIN(bugprone-unchecked-optional-access)
      proposal_data.log_queue_entry_observation(base_item_.addrkey(), index,
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
  // else validation mode
  assert(std::holds_alternative<ValidationModeData>(execution_mode_data));
  auto &validation_data = std::get<ValidationModeData>(execution_mode_data);
  auto bit_res = validation_data.consume_nondeterministic_bit();
  if (!bit_res.has_value()) {
    return SystemError{SystemErrorCode::kValidationFailure};
  }
  if (bit_res->get()) {
    auto res =
        validation_data.consume_queue_entry_observation(base_item_.addrkey(),
                                                        index);
    if (!res.has_value()) {
      return SystemError{SystemErrorCode::kValidationFailure};
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

void
NativeTokenBalanceItem::add_deterministic_native_token_balance(int128_t amount)
{
  assert(!integer_add_overflow<int128_t>(native_token_balance_, amount));
  native_token_balance_ += amount;
  // validate performs the nonnegative check

  // In no sensible codepath should a user be able to get more of the native
  // tokens in one place than exist in the entire supply.
  assert(native_token_balance_ <=
         int128_t{kTotalHogSupply} * int128_t{kHogDivisibility});
}

void
NativeTokenBalanceItem::add_nondeterministic_native_token_lock(
    Nondeterministic<int128_t> const &amount)
{

  int128_t new_sub = amount.get();
  assert(new_sub >= 0);
  assert(new_sub <= int128_t{kTotalHogSupply} * int128_t{kHogDivisibility});

  additional_subtract_amount_.get() += new_sub;
}

StorageDelta
NativeTokenBalanceItem::get_delta() const
{
  assert(additional_subtract_amount_.get() >= 0);
  int128_t subtract_amount =
      native_token_balance_ - initial_native_token_balance_;
  int128_t additional_sub = additional_subtract_amount_.get();

  StorageDelta out(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
  out.nnint() =
      NonnegativeInt128SetAddArgs{.set_value =
                                      to_xdr(initial_native_token_balance_),
                                  .add_value =
                                      to_xdr(subtract_amount - additional_sub)};
  // TODO(geoff):
  // We could conceivably avoid this delta if out.add_value == 0.
  // I'm skipping this optimization for now, because that condition
  // depends on the nondeterministic data in this item,
  // and I want to limit how that nondeterministic data is incorporated
  // downstream until we have a full picture the rest of the system. (E.g. --
  // one failure case would be to have a contract be deleted, which could delete
  // the corresponding db item --- and that would be a conflict with this delta.
  // So lets hold off on this optimization until we at least figure out contract
  // deletion behavior).
  return out;
}

StorageDelta
ReplayCacheItem::get_delta() const
{
  auto delta = StorageDelta(StorageDeltaType::kDeltaTypeReplayCacheModify);
  delta.replay_cache_modify() = ReplayCacheModifyArgs{
      .minimum_threshold = metadata_.minimum_threshold
  };
  delta.replay_cache_modify().nonces.reserve(inserted_nonces_.size());
  for (auto nonce : inserted_nonces_) {
    delta.replay_cache_modify().nonces.push_back(nonce);
  }
  return delta;
}

std::optional<ItemViewType>
UnmodifiedItem::get_item_type() const
{
  return base_item_.type();
}

void
ItemView::debug_assert_view_types(SnapshotStorageItem const &base_item,
                                  ItemViewType item_type)
{
  auto const &addrkey = base_item.addrkey();
  if (item_type == ItemViewType::kNativeTokenBalance) {
    assert(addrkey.key.key_type() ==
           StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance);
  }
  else {
    assert(addrkey.key.key_type() !=
           StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance);
  }
}

} // namespace hogd
