#pragma once

#include "hogd/base/types.h"
#include "hogd/storage_item/retrocausal_queue.h"

namespace hogd {

// These numbers are totally arbitrary.
// TODO(geoff): calibrate
constexpr static int128_t kBaseItemCost = 12300;
constexpr static int128_t kBytestringCostPerByte = 5;
constexpr static int128_t kRCQCostPerEntry = 5;
constexpr static int128_t kRCQCostPerBlock = 10;
constexpr static int128_t kReplayCacheCostPerEntry = 5;

constexpr static int128_t
bytestring_cost(size_t bytestring_len)
{
  return bytestring_len * kBytestringCostPerByte;
};

constexpr static int128_t
retrocausal_queue_cost(RetrocausalQueueStats const &stats)
{
  return (stats.active_size * kRCQCostPerEntry) +
         (stats.active_block_count * kRCQCostPerBlock);
}

constexpr static int128_t
replay_cache_cost(size_t replay_cache_size)
{
  return replay_cache_size * kReplayCacheCostPerEntry;
}

} // namespace hogd
