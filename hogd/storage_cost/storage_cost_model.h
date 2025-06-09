#pragma once

#include "hogd/base/types.h"

namespace hogd {

struct StorageCostModel {
  constexpr StorageCostModel(int128_t base_item_cost,
                             int128_t bytestring_cost_per_byte,
                             int128_t rcq_cost_per_entry,
                             int128_t rcq_cost_per_block,
                             int128_t replay_cache_cost_per_entry)
    : kBaseItemCost(base_item_cost),
      kBytestringCostPerByte(bytestring_cost_per_byte),
      kRCQCostPerEntry(rcq_cost_per_entry), kRCQCostPerBlock(rcq_cost_per_block),
      kReplayCacheCostPerEntry(replay_cache_cost_per_entry)
  {}

  int128_t get_bytestring_cost(size_t bytestring_len) const;
  int128_t get_retrocausal_queue_cost(int64_t active_queue_size,
                                      uint64_t active_block_count) const;
  int128_t get_replay_cache_cost(size_t replay_cache_size) const;

  StorageCostModel(const StorageCostModel &) = delete;
  StorageCostModel &operator=(const StorageCostModel &) = delete;
  StorageCostModel(StorageCostModel &&) = delete;
  StorageCostModel &operator=(StorageCostModel &&) = delete;

  const int128_t kBaseItemCost;
  const int128_t kBytestringCostPerByte;
  const int128_t kRCQCostPerEntry;
  const int128_t kRCQCostPerBlock;
  const int128_t kReplayCacheCostPerEntry;
};

} // namespace hogd
