#include "hogd/storage_cost/storage_cost_model.h"

#include "hogd/base/types.h"

namespace hogd {

int128_t StorageCostModel::get_bytestring_cost(size_t bytestring_len) const {
    return bytestring_len * kBytestringCostPerByte;
}

int128_t StorageCostModel::get_retrocausal_queue_cost(size_t active_queue_size, size_t active_block_count) const {
    return (active_queue_size * kRCQCostPerEntry) + (active_block_count * kRCQCostPerBlock);
}

}   