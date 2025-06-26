#include "hogd/hogexec/runtime/test_util/queue_util.h"
#include "hogd/base/types.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "retrocausal_queue.hh"
#include "x23/vec.h"
#include <utility>
#include <vector>

namespace hogd {

QueueEntry
make_test_queue_entry(uint8_t value)
{
  return {value};
}
namespace {
RetrocausalQueue
add_queue_block(RetrocausalQueue const &parent, x23::Vec<QueueEntry> entries)
{
  return RetrocausalQueue(
      parent,
      RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                0,
                                            .partial_consumed_number = 0},
      RetrocausalQueueBlock{.entries = std::move(entries),
                            .block_number =
                                parent.get_current_block_number() + 1});
}
} // namespace

RetrocausalQueue
make_queue(std::vector<x23::Vec<QueueEntry>> const &blocks)
{
  RetrocausalQueue queue(0);
  for (auto const &block : blocks) {
    queue = add_queue_block(queue, block);
  }
  return queue;
}

} // namespace hogd
