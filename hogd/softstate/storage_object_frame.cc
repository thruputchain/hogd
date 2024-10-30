#include "hogd/softstate/storage_object_frame.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <optional>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/short_allocator.h"
#include "hogd/base/types.h"
#include "hogd/softstate/atomic_uint128.h"
#include "storage_object.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

struct BytestringArgs {
  const Bytestring bytestring_set_;

  bool can_accept(Bytestring const &set_arg) const
  {
    return bytestring_set_ == set_arg;
  }

  StorageObject to_object() const
  {
    StorageObject object;
    object.type(StorageObjectType::kBytestring);
    object.bytestring() = bytestring_set_;
    return object;
  }
};

struct NNInt64SetAddArgs {
  const int64_t set_value_;
  std::atomic<int64_t> total_subtracted_ = 0;
  AtomicUint128 total_added_;

  bool can_accept(int64_t const &set_arg) const
  {
    return set_value_ == set_arg;
  }

  StorageObject to_object() const
  {
    StorageObject object;
    object.type(StorageObjectType::kNonnegativeInt64);
    object.int64() = set_value_;

    object.int64() += total_subtracted_.load(std::memory_order_relaxed);

    uint64_t total_add = total_added_.get_capped();

    if (integer_add_overflow_p<int64_t>(object.int64(), total_add)) {
      object.int64() = INT64_MAX;
    }
    else {
      object.int64() += total_add;
    }

    return object;
  }
};

using variant_t = std::variant<BytestringArgs, NNInt64SetAddArgs>;

// delta_tag_and_ptr_ stores a <tag: 32 bits, ptr: 32 bits> pair.
// ptr is controlled and dereferenced within allocator.
// Tag controls concurrent access to ptr, and acts as a reference count.
// Tag has the following format: [finalized, 1 bit | number of concurrent,
// unfinished mod count: 31 bits]
//
// try_reserve increments the unfinished mod count, commit sets the finalized
// bit, and rewind reduces the unfinished mod count, if finalized is unset.
//
// If a thread decrements the unfinished mod count
// to 0, then it clears the ptr.  (This is why calling revert() before
// try_reserve is bad).
//
// Threads set the ptr to whatever their modification needs if the unfinished
// mod count is 0.
//
// Otherwise, they either proceed or abort immediately (based on whether the ptr
// that is present matches what the modification needs).
//
// Frees are deferred to the end of a block, to prevent UAF.

namespace {

// Inside this allocator is a threadlocal free list.
// Frees are deferred to the end of the round.
// WARNING: when a thread terminates, its threadlocal allocations are not
// immediately garbage collected.  This should only be accessed by a static set
// of worker threads.
//
// These threadlocal allocations do not steal from each other.  I expect worker
// threads to receive work schedules in a roungly evenly distributed way, so the
// size of each freelist (roughly speaking, the number of times a thread first
// touches a storage object in a round) should not vary too much across threads.
RoundAllocator<variant_t> allocator;

bool
is_finalized_tag(uint32_t tag)
{
  return (tag & 0x8000'0000) != 0;
}

void
increase_active_thread_count(uint32_t &tag)
{
  tag++;
  assert(tag != 0);
}

void
decrease_active_thread_count(uint32_t &tag)
{
  tag--;
}

uint64_t
construct_tag_and_ptr(uint32_t tag, uint32_t ptr)
{
  return (uint64_t{tag} << 32) + ptr;
}

variant_t *
get_variant(std::atomic<uint64_t> &atomic_tag_and_ptr)
{
  uint64_t tag_and_ptr = atomic_tag_and_ptr.load(std::memory_order_relaxed);

  uint32_t short_ptr = tag_and_ptr & 0xFFFF'FFFF;

  return allocator.get(short_ptr);
}

// get variant should only be called in a context in which an active reservation
// for the specified type has been taken on the variant
template<typename VariantArm>
VariantArm *
get_variant_arm_unchecked(std::atomic<uint64_t> &atomic_tag_and_ptr)
{
  variant_t *ptr = get_variant(atomic_tag_and_ptr);

  assert(!!ptr);

  return &std::get<VariantArm>(*ptr);
}

void
free_variant(std::atomic<uint64_t> &atomic_tag_and_ptr)
{
  uint64_t tag_and_ptr = atomic_tag_and_ptr.load(std::memory_order_relaxed);

  uint32_t short_ptr = tag_and_ptr & 0xFFFF'FFFF;

  allocator.deferred_free(short_ptr);

  atomic_tag_and_ptr.store(0, std::memory_order_relaxed);
}

template<typename VariantArm, typename... VariantArgs>
VariantArm *
try_set_variant(std::atomic<uint64_t> &atomic_tag_and_ptr,
                VariantArgs const &...args)
{
  uint64_t tag_and_ptr = atomic_tag_and_ptr.load(std::memory_order_acquire);
  while (true) {

    uint32_t tag = tag_and_ptr >> 32;
    uint32_t short_ptr = tag_and_ptr & 0xFFFF'FFFF;

    variant_t *ptr = allocator.get(short_ptr);

    if (ptr) {
      assert(tag != 0);

      if (!std::holds_alternative<VariantArm>(*ptr)) {
        return nullptr;
      }
      if (!std::get<VariantArm>(*ptr).can_accept(args...)) {
        return nullptr;
      }

      if (is_finalized_tag(tag)) {
        return &std::get<VariantArm>(*ptr);
      }

      increase_active_thread_count(tag);
      uint64_t new_tag_and_ptr = construct_tag_and_ptr(tag, short_ptr);

      if (atomic_tag_and_ptr.compare_exchange_strong(
              tag_and_ptr, new_tag_and_ptr, std::memory_order_relaxed)) {
        return &std::get<VariantArm>(*ptr);
      }
    }
    else {
      auto [new_short_ptr, new_ptr] =
          allocator.construct(std::in_place_type<VariantArm>, args...);

      assert(!!new_ptr);

      uint32_t new_tag = 0;
      increase_active_thread_count(new_tag);

      uint64_t new_tag_and_ptr = construct_tag_and_ptr(new_tag, new_short_ptr);

      if (atomic_tag_and_ptr.compare_exchange_strong(
              tag_and_ptr, new_tag_and_ptr, std::memory_order_release)) {
        return &std::get<VariantArm>(*new_ptr);
      }
    }
    cpu_relax();
  }
}

void
release_variant(std::atomic<uint64_t> &atomic_tag_and_ptr)
{
  uint64_t old_tag_and_ptr = atomic_tag_and_ptr.load(std::memory_order_relaxed);
  while (true) {
    uint32_t tag = old_tag_and_ptr >> 32;
    uint32_t short_ptr = old_tag_and_ptr & 0xFFFF'FFFF;

    if (is_finalized_tag(tag)) {
      return;
    }

    uint32_t new_ptr = short_ptr;

    decrease_active_thread_count(tag);

    if (tag == 0) {
      new_ptr = 0;
    }

    uint64_t new_tag_and_ptr = construct_tag_and_ptr(tag, new_ptr);

    if (atomic_tag_and_ptr.compare_exchange_strong(old_tag_and_ptr,
                                                   new_tag_and_ptr,
                                                   std::memory_order_release)) {
      if (tag == 0) {
        allocator.deferred_free(short_ptr);
      }
      return;
    }
    cpu_relax();
  }
}

void
finalize_variant(std::atomic<uint64_t> &atomic_tag_and_ptr)
{
  // invariant: if you finalize, you must have called try_set earlier without
  // calling release-- so there is an active refcnt.
  atomic_tag_and_ptr.fetch_or(0x8000'0000'0000'0000, std::memory_order_relaxed);
}

} // namespace

StorageObjectFrame::StorageObjectFrame(
    std::optional<StorageObject> const &snapshot_state)
  : committed_base_object_(snapshot_state), deleted_last_(false)
{}

bool
StorageObjectFrame::type_guard(StorageObjectType expected_type) const
{
  if (committed_base_object_) {
    return committed_base_object_->type() == expected_type;
  }
  return true;
}

StorageReserveResult
StorageObjectFrame::try_reserve_bytestring_set(Bytestring const &bytestring_set)
{
  if (!type_guard(StorageObjectType::kBytestring)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  if ([[maybe_unused]] auto *args =
          try_set_variant<BytestringArgs>(delta_tag_and_ptr_, bytestring_set)) {
    assert(args->bytestring_set_ == bytestring_set);

    return StorageReserveResult::kSuccess;
  }
  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_bytestring_set()
{
  release_variant(delta_tag_and_ptr_);
}

StorageReserveResult
StorageObjectFrame::try_reserve_nnint64_set_add(int64_t set_value,
                                                int64_t add_value)
{
  if (!type_guard(StorageObjectType::kNonnegativeInt64)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  if (auto *args =
          try_set_variant<NNInt64SetAddArgs>(delta_tag_and_ptr_, set_value)) {
    assert(args->set_value_ == set_value);

    if (add_value >= 0) {
      args->total_added_ += add_value;
      return StorageReserveResult::kSuccess;
    }

    if (set_value < 0) { // and add_value < 0
      release_variant(delta_tag_and_ptr_);

      return StorageReserveResult::kDefiniteFailure;
    }

    int64_t expect_sub =
        args->total_subtracted_.load(std::memory_order_relaxed);

    while (true) {
      int64_t desired_sub = 0;

      if (integer_add_overflow(expect_sub, add_value, &desired_sub)) {
        release_variant(delta_tag_and_ptr_);
        return StorageReserveResult::kDefiniteFailure;
      }

      // Cannot overflow.
      // set_value > 0 and desired_sub < 0.  So worst case
      // we get 1 + INT64_MIN, which does not overflow.
      if (set_value + desired_sub < 0) {
        release_variant(delta_tag_and_ptr_);
        return StorageReserveResult::kDefiniteFailure;
      }

      if (args->total_subtracted_.compare_exchange_strong(
              expect_sub, desired_sub, std::memory_order_relaxed)) {
        return StorageReserveResult::kSuccess;
      }
      cpu_relax();
    }
  }
  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_nnint64_set_add(int64_t add_value)
{
  auto *args = get_variant_arm_unchecked<NNInt64SetAddArgs>(delta_tag_and_ptr_);

  if (add_value >= 0) {
    args->total_added_ -= add_value;
  }
  else {
    args->total_subtracted_.fetch_sub(add_value, std::memory_order_relaxed);
  }

  release_variant(delta_tag_and_ptr_);
}

StorageReserveResult
StorageObjectFrame::try_reserve_delta(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    return StorageReserveResult::kSuccess;
  case StorageDeltaType::kBytestringSet:
    return try_reserve_bytestring_set(delta.bytestring());
  case StorageDeltaType::kNonnegativeInt64SetAdd:
    return try_reserve_nnint64_set_add(delta.nnint64().set_value,
                                       delta.nnint64().add_value);
  default:
    return StorageReserveResult::kDefiniteFailure;
  }
}

void
StorageObjectFrame::commit_delta(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    deleted_last_.store(true, std::memory_order_relaxed);
    return;
  default:
    finalize_variant(delta_tag_and_ptr_);
    // no op
    return;
  }
}

void
StorageObjectFrame::revert_delta(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    return;
  case StorageDeltaType::kBytestringSet:
    revert_bytestring_set();
    return;
  case StorageDeltaType::kNonnegativeInt64SetAdd:
    revert_nnint64_set_add(delta.nnint64().add_value);
    return;
  }
}

std::optional<StorageObject> const &
StorageObjectFrame::get_committed_object() const
{
  return committed_base_object_;
}

void
StorageObjectFrame::clear_modifications()
{
  deleted_last_ = false;
  free_variant(delta_tag_and_ptr_);
}

void
StorageObjectFrame::commit_round(uint64_t round_number)
{
  x23::Defer cleanup([&] {
    clear_modifications();
    allocator.release_deferred_frees(round_number);
  });

  auto *variant = get_variant(delta_tag_and_ptr_);

  if (deleted_last_) {
    committed_base_object_ = std::nullopt;
    return;
  }

  if (!variant) {
    // no committed modifications.
    return;
  }

  committed_base_object_ =
      std::visit([](auto const &arm) { return arm.to_object(); }, *variant);
}

void
StorageObjectFrame::revert_round(uint64_t round_number)
{
  clear_modifications();
  allocator.release_deferred_frees(round_number);
}

StorageObjectFrame::~StorageObjectFrame()
{
  free_variant(delta_tag_and_ptr_);
}

} // namespace hogd
