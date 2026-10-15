#pragma once

// Accessors for RetrocausalQueueDelta::consume, the boxed (#[cpp(box)])
// RetrocausalQueueConsume union.

#include <utility>

#include "hogd/base/log.h"
#include "retrocausal_queue.hh"
#include "x23/vec.h"

namespace hogd {

using enum RetrocausalQueueConsumeStatus;

using RcqConsumedEntries =
    x23::Vec<QueueEntry, kRetrocausalQueueBlockEntriesMaxLen>;

inline RetrocausalQueueConsumeStatus
rcq_consume_status(RetrocausalQueueDelta const &delta)
{
  return delta.consume->status();
}

// Whether the transaction deleted or overwrote the key before using it as a
// queue, leaving HogDb to consume the committed queue.
inline bool
rcq_is_replace(RetrocausalQueueDelta const &delta)
{
  auto status = rcq_consume_status(delta);
  return status == kRcqReplace || status == kRcqReplaceConsume;
}

// Whether the delta consumes explicitly. A kRcqReplace delta may still
// consume implicitly; see RetrocausalQueueLock::claims_consumer.
inline bool
rcq_has_explicit_consume(RetrocausalQueueDelta const &delta)
{
  auto status = rcq_consume_status(delta);
  return status == kRcqConsume || status == kRcqReplaceConsume;
}

// The kRcqConsume arm, or null.
inline RetrocausalQueueConsumeDelta const *
rcq_consume_delta(RetrocausalQueueDelta const &delta)
{
  if (rcq_consume_status(delta) != kRcqConsume) {
    return nullptr;
  }
  return &delta.consume->delta();
}

inline RetrocausalQueueConsumeDelta *
rcq_consume_delta(RetrocausalQueueDelta &delta)
{
  if (rcq_consume_status(delta) != kRcqConsume) {
    return nullptr;
  }
  return &delta.consume->delta();
}

// The current-block entries an explicit consume carries, or empty.
inline RcqConsumedEntries const &
rcq_current_block_consumed(RetrocausalQueueDelta const &delta)
{
  static const RcqConsumedEntries kNone;
  switch (rcq_consume_status(delta)) {
  case kRcqConsume:
    return delta.consume->delta().current_block_consumed;
  case kRcqReplaceConsume:
    return delta.consume->current_block_consumed();
  default:
    return kNone;
  }
}

// As above, for writing; the delta must have an explicit consume.
inline RcqConsumedEntries &
rcq_mutable_current_block_consumed(RetrocausalQueueDelta &delta)
{
  switch (rcq_consume_status(delta)) {
  case kRcqConsume:
    return delta.consume->delta().current_block_consumed;
  case kRcqReplaceConsume:
    return delta.consume->current_block_consumed();
  default:
    panic("rcq_mutable_current_block_consumed: delta has no explicit consume");
  }
}

// Sets the consume status, keeping the current-block entries when both the
// old and the new status carry them.
inline RetrocausalQueueConsume &
rcq_set_consume_status(RetrocausalQueueDelta &delta,
                       RetrocausalQueueConsumeStatus status)
{
  auto old_status = delta.consume->status();
  if (old_status == status) {
    return *delta.consume;
  }
  RcqConsumedEntries carried;
  if (old_status == kRcqConsume) {
    carried = std::move(delta.consume->delta().current_block_consumed);
  }
  else if (old_status == kRcqReplaceConsume) {
    carried = std::move(delta.consume->current_block_consumed());
  }
  delta.consume->status(status);
  if (status == kRcqConsume) {
    delta.consume->delta().current_block_consumed = std::move(carried);
  }
  else if (status == kRcqReplaceConsume) {
    delta.consume->current_block_consumed() = std::move(carried);
  }
  return *delta.consume;
}

// Makes the delta an ordinary explicit consume and returns it.
inline RetrocausalQueueConsumeDelta &
rcq_set_consume(RetrocausalQueueDelta &delta)
{
  return rcq_set_consume_status(delta, kRcqConsume).delta();
}

} // namespace hogd
