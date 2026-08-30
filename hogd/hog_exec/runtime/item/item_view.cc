#include "hogd/hog_exec/runtime/item/item_view.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <variant>

#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/runtime/item/item_view_type.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
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

// Replay reached a new observation but the recorded sequence is exhausted
// (validation/viewing). If the proposer stopped at exactly the cap, replay
// agrees with an empty read; otherwise the proposer recorded fewer observations
// than this contract consumes -- a malformed proposal, i.e. a validation
// failure. (#2894)
SystemStatus<const QueueEntry *>
replay_exhausted_at_cap(bool cap_exceeded)
{
  if (cap_exceeded) {
    return nullptr;
  }
  return SystemError{SystemErrorCode::ValidationFailure};
}

} // namespace

std::pair<std::optional<StorageDelta>, StorageDeltaFlagSet>
ItemView::get_delta(std::span<QueueEntry const> observed_entries) const
{
  auto delta = std::visit(
      [observed_entries](auto const &v) -> std::optional<StorageDelta> {
        if constexpr (requires { v.get_delta(observed_entries); }) {
          return v.get_delta(observed_entries);
        }
        else {
          return v.get_delta();
        }
      },
      delta_args_);

  return {std::move(delta), flags_};
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
  // locally_consumed_index_ is a uint16; the per-block consumed count is
  // bounded by kRetrocausalQueueBlockEntriesMaxLen (see the static_assert in
  // the header). Narrow explicitly so the conversion is intentional, not an
  // implicit uint32 -> uint16 truncation.
  assert(consumed_entries.size() <=
         static_cast<size_t>(kRetrocausalQueueBlockEntriesMaxLen));
  auto retrocausally_consumed_count =
      static_cast<uint16_t>(consumed_entries.size());

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
SystemStatus<const QueueEntry *>
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

  // Bound past-block reads per transaction. Each past-block read pins a full
  // decoded block (up to ~64 KiB) in the shared, block-lifetime snapshot cache
  // while charging zero direct host gas, and this path runs before the
  // concurrent-observation cap; without a bound a transaction could turn many
  // cheap distinct-key reads into unbounded host memory (#3531). Check the cap
  // *before* get_next_block so a read past the cap does not load or retain a
  // new block in the shared cache; the crossing read (below) is the last one
  // that resolves a block. load_next_queue_entry surfaces the handleable
  // RetrocausalQueueReadLimitReached error either way, keeping proposal and
  // validation in lockstep.
  if (past_block_rcq_read_cap_exceeded(execution_mode_data)) {
    return nullptr;
  }

  auto next_block_status = get_next_block(block_index);
  if (!next_block_status.has_value()) {
    return next_block_status.error();
  }
  auto const *next_block = next_block_status.value();

  if (next_block) {
    // Count this past-block read. The read that crosses the cap consumes
    // nothing and leaves the cursor unchanged; every subsequent read
    // short-circuits above before loading another block.
    if (!record_past_block_rcq_read(execution_mode_data)) {
      return nullptr;
    }

    // Recovered/corrupt metadata can point the cursor past the block, or the
    // block can be empty; fail-stop rather than index OOB. (#2944, #2978)
    if (partial_consume_quantity >= next_block->entries.size()) {
      error("Unrecoverable: recovered RCQ consume cursor {} is past block {} "
            "entry count {}",
            partial_consume_quantity, next_block->block_number,
            next_block->entries.size());
      return SystemError{SystemErrorCode::Unrecoverable};
    }

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

    // Pointer into the snapshot block, which outlives the transaction.
    return &next_block->entries[partial_consume_quantity];
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
    // Pointer into this item's own self-insert delta.
    return self_consume_res;
  }

  // Step 3: try to consume concurrent entries not from this transaction
  auto concurrent_entry =
      get_next_concurrent_entry(locally_consumed_index_, execution_mode_data);
  if (!concurrent_entry.has_value()) {
    return concurrent_entry.error();
  }
  const QueueEntry *concurrent_ptr = concurrent_entry.value();

  if (concurrent_ptr != nullptr) {
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
      // Copies into the consume delta (a distinct buffer from the observation
      // storage the pointer aliases, so this does not invalidate it).
      queue_delta_.consume->current_block_consumed.push_back(*concurrent_ptr);
    }

    return concurrent_ptr;
  }

  // Step 4: nothing to consume
  return nullptr;
}

StorageDelta
RetrocausalQueueItem::get_delta(
    std::span<QueueEntry const> observed_entries) const
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

  if (!observed_entries.empty()) {
    delta.retrocausal_queue().observed_concurrent_entries = {
        observed_entries.begin(), observed_entries.end()};
  }

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

// Resolve the next concurrent (non-self, non-past-block) RCQ observation.
// Returns a pointer to the entry to consume, nullptr (nothing to consume: empty
// read or cap reached), or a ValidationFailure on malformed replay data. This
// is the single location aware of proposal-vs-validation-vs-viewing for RCQ
// reads; the get_next_item caller only sees the uniform pointer result. The
// returned pointer aliases into owned observation storage (see get_next_item).
SystemStatus<const QueueEntry *>
RetrocausalQueueItem::get_next_concurrent_entry(
    uint32_t index, ExecutionModeData &execution_mode_data) const
{
  assert(base_item_);
  auto const &addrkey = base_item_->addrkey();

  if (auto *proposal_data =
          std::get_if<ProposalModeData>(&execution_mode_data)) {
    // Once the cap is hit, record nothing further and stop probing the queue
    // (#2894).
    if (proposal_data->observation_cap_exceeded()) {
      return nullptr;
    }
    auto out = base_item_->get_concurrent_rcq_entry(index);
    if (!out.has_value()) {
      return out.error();
    }
    // Reference, not a copy: QueueEntry is heap-backed, so this avoids an
    // allocation the original also avoided.
    auto const &live_opt = out.value().get();
    if (!live_opt.has_value()) {
      proposal_data->log_nondeterministic_bit({false});
      return nullptr;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    QueueEntry const &live = *live_opt;
    // The entry the caller may consume is exactly what recording hands back: a
    // pointer to the stored observation, or nullptr iff logging crossed the cap
    // (nothing recorded). An unrecorded entry is therefore unreturnable, so a
    // cap-crossing read applies no proposal-only consume that validation replay
    // would not reproduce (#3043). The bit is logged first because the bit and
    // observation streams are independent, so their relative append order does
    // not matter.
    proposal_data->log_nondeterministic_bit({true});
    // Unwrap the nondeterministic tag: the nondeterminism was logged above, and
    // the recorded pointer is nullptr iff logging crossed the cap.
    auto recorded =
        proposal_data->log_queue_entry_observation(addrkey, index, live);
    return recorded.get();
  }

  if (auto *viewing_data = std::get_if<ViewingModeData>(&execution_mode_data)) {
    if (viewing_data->observation_cap_exceeded()) {
      return nullptr;
    }
    // Bind to a local: Nondeterministic::get() takes an lvalue object argument,
    // so it cannot be called on the returned temporary directly.
    auto queue_bit = viewing_data->view_query_queue_observation_bit();
    if (!queue_bit.get()) {
      // no queue entry available
      return nullptr;
    }
    // Bind before .get(): Nondeterministic::get() needs an lvalue.
    auto observed = viewing_data->view_queue_entry_observation(addrkey, index);
    if (const QueueEntry *entry = observed.get()) {
      return entry;
    }
    return replay_exhausted_at_cap(viewing_data->observation_cap_exceeded());
  }

  // else validation mode
  assert(std::holds_alternative<ValidationModeData>(execution_mode_data));
  auto &validation_data = std::get<ValidationModeData>(execution_mode_data);
  if (validation_data.observation_cap_exceeded()) {
    return nullptr;
  }
  auto bit_res = validation_data.consume_nondeterministic_bit();
  if (!bit_res.has_value()) {
    // Bit stream exhausted: a malformed proposal.
    return SystemError{SystemErrorCode::ValidationFailure};
  }
  if (!bit_res->get()) {
    return nullptr;
  }
  // Bind before .get(): Nondeterministic::get() needs an lvalue.
  auto observed =
      validation_data.consume_queue_entry_observation(addrkey, index);
  if (const QueueEntry *entry = observed.get()) {
    return entry;
  }
  return replay_exhausted_at_cap(validation_data.observation_cap_exceeded());
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
