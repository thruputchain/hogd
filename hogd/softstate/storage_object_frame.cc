#include "hogd/softstate/storage_object_frame.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/softstate/uint128_reservation.h"
#include "storage_object.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

bool
StorageObjectFrame::NNInt64SetAddArgs::can_accept(int64_t const &set_arg) const
{
  return set_value_ == set_arg;
}

StorageObject
StorageObjectFrame::NNInt64SetAddArgs::to_object(
    std::optional<StorageObject> const & /*unused*/) const
{
  StorageObject object;
  object.type(StorageObjectType::kNonnegativeInt64);
  object.int64() = set_value_;

  object.int64() += total_subtracted_;

  uint64_t total_add = total_added_.get_capped();

  if (integer_add_overflow_p<int64_t>(object.int64(), total_add)) {
    object.int64() = INT64_MAX;
  }
  else {
    object.int64() += total_add;
  }

  return object;
}

StorageObject
StorageObjectFrame::ReplayCacheArgs::to_object(
    std::optional<StorageObject> const &previous_object) const
{
  StorageObject out;
  if (previous_object) {
    out = *previous_object;
  }
  else {
    out.type(StorageObjectType::kReplayCache);
    out.replay_cache().metadata.clear_threshold = 0;
  }

  // TODO(geoff): this is a very inefficient but correct way of doing the data
  // structures This should be a set or a BTree
  for (auto val : new_nonces_) {
    out.replay_cache().nonces.push_back(val);
  }

  std::sort(out.replay_cache().nonces.begin(), out.replay_cache().nonces.end(),
            std::greater<>());

  if (new_max_clear_threshold_.has_value()) {

    assert(new_max_clear_threshold_.value() >
           out.replay_cache().metadata.clear_threshold);

    // TODO(geoff) lazy - swap for a binary search
    for (uint32_t i = 0; i < out.replay_cache().nonces.size(); i++) {
      if (out.replay_cache().nonces[i] < new_max_clear_threshold_.value()) {
        out.replay_cache().nonces.resize(i);
        break;
      }
    }

    out.replay_cache().metadata.clear_threshold = *new_max_clear_threshold_;
  }

  out.replay_cache().metadata.current_size = out.replay_cache().nonces.size();

  return out;
}

void
StorageObjectFrame::release_variant(RefcountedVariant &variant)
{
  assert(variant.refcount_ > 0);
  variant.refcount_--;
}

template<typename VariantArm, typename... VariantArgs>
VariantArm *
StorageObjectFrame::try_set_variant(RefcountedVariant &variant,
                                    VariantArgs const &...args)
{
  if (variant.refcount_ == 0) {
    variant.variant_.emplace<VariantArm>(args...);
  }

  if (!std::holds_alternative<VariantArm>(variant.variant_)) {
    return nullptr;
  }

  if (!std::get<VariantArm>(variant.variant_).can_accept(args...)) {
    return nullptr;
  }

  variant.refcount_++;

  return &std::get<VariantArm>(variant.variant_);
}

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

  auto v = variant_.take();

  if ([[maybe_unused]] auto *args =
          try_set_variant<BytestringArgs>(*v, bytestring_set)) {
    assert(args->bytestring_set_ == bytestring_set);

    return StorageReserveResult::kSuccess;
  }
  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_bytestring_set()
{
  release_variant(*variant_.take());
}

StorageReserveResult
StorageObjectFrame::try_reserve_nnint64_set_add(int64_t set_value,
                                                int64_t add_value)
{
  if (!type_guard(StorageObjectType::kNonnegativeInt64)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  auto v = variant_.take();

  if (auto *args = try_set_variant<NNInt64SetAddArgs>(*v, set_value)) {
    assert(args->set_value_ == set_value);

    if (add_value >= 0) {
      args->total_added_ += add_value;
      return StorageReserveResult::kSuccess;
    }

    if (set_value < 0) { // and add_value < 0
      release_variant(*v);

      return StorageReserveResult::kDefiniteFailure;
    }

    int64_t desired_subtracted = 0;

    if (integer_add_overflow(args->total_subtracted_, add_value,
                             &desired_subtracted)) {
      release_variant(*v);
      return StorageReserveResult::kDefiniteFailure;
    }

    // Cannot overflow.
    // set_value > 0 and desired_sub < 0.  So worst case
    // we get 1 + INT64_MIN, which does not overflow.
    if (set_value + desired_subtracted < 0) {
      release_variant(*v);
      return StorageReserveResult::kDefiniteFailure;
    }

    args->total_subtracted_ = desired_subtracted;
    return StorageReserveResult::kSuccess;
  }

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_nnint64_set_add(int64_t add_value)
{
  auto v = variant_.take();

  auto &args = get_variant_arm_unchecked<NNInt64SetAddArgs>(*v);

  if (add_value >= 0) {
    args.total_added_ -= add_value;
  }
  else {
    args.total_subtracted_ -= add_value;
  }

  release_variant(*v);
}

StorageReserveResult
StorageObjectFrame::try_reserve_replay_cache_inserts(
    x23::Vec<uint64_t> const &nonces)
{
  if (!type_guard(StorageObjectType::kReplayCache)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  if (committed_base_object_) {
    assert(committed_base_object_->type() == StorageObjectType::kReplayCache);

    for (auto nonce : nonces) {
      if (committed_base_object_->replay_cache().metadata.clear_threshold >
          nonce) {
        return StorageReserveResult::kDefiniteFailure;
      }
      for (auto v : committed_base_object_->replay_cache().nonces) {
        if (v == nonce) {
          return StorageReserveResult::kDefiniteFailure;
        }
      }
    }
  }

  auto v = variant_.take();

  if (auto *args = try_set_variant<ReplayCacheArgs>(*v)) {

    for (auto nonce : nonces) {
      if (args->new_nonces_.contains(nonce)) {
        release_variant(*v);
        return StorageReserveResult::kDefiniteFailure;
      }
    }

    for (auto nonce : nonces) {
      args->new_nonces_.insert(nonce);
    }

    return StorageReserveResult::kSuccess;
  }

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_replay_cache_inserts(
    x23::Vec<uint64_t> const &nonces)
{
  auto v = variant_.take();
  auto &args = get_variant_arm_unchecked<ReplayCacheArgs>(*v);

  for (auto nonce : nonces) {
    [[maybe_unused]] auto res = args.new_nonces_.erase(nonce);
    assert(res == 1);
  }

  release_variant(*v);
}

StorageReserveResult
StorageObjectFrame::try_reserve_replay_cache_clear(uint64_t threshold)
{
  if (!type_guard(StorageObjectType::kReplayCache)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  if (committed_base_object_) {
    assert(committed_base_object_->type() == StorageObjectType::kReplayCache);

    if (committed_base_object_->replay_cache().metadata.clear_threshold >
        threshold) {
      return StorageReserveResult::kDefiniteFailure;
    }
  }

  auto v = variant_.take();

  if (try_set_variant<ReplayCacheArgs>(*v)) {

    return StorageReserveResult::kSuccess;
  }

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::commit_replay_cache_clear(uint64_t threshold)
{
  auto v = variant_.take();
  auto &args = get_variant_arm_unchecked<ReplayCacheArgs>(*v);

  if (args.new_max_clear_threshold_.has_value()) {
    args.new_max_clear_threshold_ =
        std::max(threshold, *args.new_max_clear_threshold_);
  }
  else {
    args.new_max_clear_threshold_ = threshold;
  }
}

void
StorageObjectFrame::revert_replay_cache_clear()
{
  auto v = variant_.take();
  release_variant(*v);
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
  case StorageDeltaType::kReplayCacheInsert:
    return try_reserve_replay_cache_inserts(delta.replay_nonces());
  case StorageDeltaType::kReplayCacheClear:
    return try_reserve_replay_cache_clear(delta.replay_clear_threshold());
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
  case StorageDeltaType::kReplayCacheClear:
    commit_replay_cache_clear(delta.replay_clear_threshold());
  default:
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
  case StorageDeltaType::kReplayCacheInsert:
    revert_replay_cache_inserts(delta.replay_nonces());
    return;
  case StorageDeltaType::kReplayCacheClear:
    revert_replay_cache_clear();
    return;
  }
}

std::optional<StorageObject> const &
StorageObjectFrame::get_committed_object() const
{
  return committed_base_object_;
}

std::optional<StrippedStorageObject>
StorageObjectFrame::get_stripped_committed_object() const
{
  if (!committed_base_object_) {
    return std::nullopt;
  }

  StrippedStorageObject obj(committed_base_object_->type());
  switch (obj.type()) {
  case StorageObjectType::kBytestring:
    obj.bytestring() = committed_base_object_->bytestring();
    break;
  case StorageObjectType::kNonnegativeInt64:
    obj.int64() = committed_base_object_->int64();
    break;
  case StorageObjectType::kBoundedHashset:
    obj.hashset() = committed_base_object_->hashset();
    break;
  case StorageObjectType::kReplayCache:
    obj.replay_cache_metadata() =
        committed_base_object_->replay_cache().metadata;
    break;
  }
  return obj;
}

void
StorageObjectFrame::clear_modifications()
{
  deleted_last_ = false;
  variant_->refcount_ = 0;
}

void
StorageObjectFrame::commit_round()
{
  x23::Defer cleanup([&] { clear_modifications(); });

  auto v = variant_.take();

  if (deleted_last_) {
    committed_base_object_ = std::nullopt;
    return;
  }

  if (v->refcount_ == 0) {
    // no committed modifications.
    return;
  }

  committed_base_object_ = std::visit(
      [&](auto const &arm) { return arm.to_object(committed_base_object_); },
      v->variant_);
}

void
StorageObjectFrame::revert_round()
{
  clear_modifications();
}

} // namespace hogd
