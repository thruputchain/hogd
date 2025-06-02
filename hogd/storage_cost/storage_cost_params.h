#pragma once

#include "hogd/base/types.h"
#include <variant>

namespace hogd {

struct StorageCostModel;

// Storage cost parameters.
// These are computed from a StorageItem.
// Actual storage costs are computed from these parameters,
// using the current StorageCostModel.
class StorageCostParams {
public:
    struct BytestringCostParams {
        size_t bytestring_len;
        int128_t get_cost(StorageCostModel const &model) const;
    };
    struct RetrocausalQueueCostParams {
        size_t active_queue_size;
        size_t active_block_count;
        int128_t get_cost(StorageCostModel const &model) const;
    };
    struct NNIntCostParams {
        static int128_t get_cost(StorageCostModel const &model);
    };
    struct DeletedCostParams {
        static int128_t get_cost(StorageCostModel const & /*unused*/) { return 0; };
    };

    template<typename T>
    StorageCostParams(T const &params) : params_(params) {}

    int128_t get_cost(StorageCostModel const &model) const;

private:

    std::variant<BytestringCostParams, RetrocausalQueueCostParams, NNIntCostParams, DeletedCostParams> params_;

};

}