#pragma once

#include "hogd/base/types.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "storage_item.hh"

namespace hogd {

constexpr static int128_t kBaseItemCost = 12300;

constexpr static int128_t
bytestring_cost(size_t bytestring_len)
{
  return bytestring_len * 5;
};

constexpr static int128_t kMaxBytestringCost =
    bytestring_cost(kBytestringMaxLen);

constexpr static int128_t
retrocausal_queue_cost(RetrocausalQueueStats const &stats)
{
  return (stats.active_size * 5) + (stats.active_block_count * 10);
}

} // namespace hogd
