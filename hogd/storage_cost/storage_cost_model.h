#pragma once

#include "hogd/base/types.h"

namespace hogd {

struct StorageCostModel {
    StorageCostModel(int128_t base_item_cost, int128_t bytestring_cost_per_byte, int128_t rcq_cost_per_entry, int128_t rcq_cost_per_block)
        : kBaseItemCost(base_item_cost), kBytestringCostPerByte(bytestring_cost_per_byte), kRCQCostPerEntry(rcq_cost_per_entry), kRCQCostPerBlock(rcq_cost_per_block) {}

    int128_t get_bytestring_cost(size_t bytestring_len) const;
    int128_t get_retrocausal_queue_cost(size_t active_queue_size, size_t active_block_count) const;

    const int128_t kBaseItemCost;
    const int128_t kBytestringCostPerByte;
    const int128_t kRCQCostPerEntry;
    const int128_t kRCQCostPerBlock;
};

}