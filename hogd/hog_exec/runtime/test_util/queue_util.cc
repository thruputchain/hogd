#include "hogd/hog_exec/runtime/test_util/queue_util.h"

#include <cstring>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "x23/vec.h"

namespace hogd {

QueueEntry
make_test_queue_entry(uint32_t value)
{
  QueueEntry entry;
  entry.resize(sizeof(value));
  std::memcpy(entry.data(), &value, sizeof(value));
  return entry;
}

TestQueue
make_queue(std::vector<x23::Vec<QueueEntry>> const &blocks)
{
  TestQueue queue{
      .item = WireStorageItem(StorageItemType::kItemTypeRetrocausalQueue),
      .blocks = {},
  };
  uint64_t live_entries = 0;
  uint64_t block_number = 0;
  for (x23::Vec<QueueEntry> entries : blocks) {
    ++block_number;
    live_entries += entries.size();
    queue.blocks.emplace(block_number,
                         RetrocausalQueueBlock{.entries = std::move(entries),
                                               .block_number = block_number});
  }
  queue.item.metadata() = RetrocausalQueueMetadata{
      .consume_state =
          RetrocausalQueuePastBlockConsumeDelta{
              .max_fully_consumed_block_number = 0,
              .partial_consumed_number = 0,
          },
      .max_live_block_num = block_number,
      .live_entries = live_entries,
  };
  return queue;
}

} // namespace hogd
