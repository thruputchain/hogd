#include "hogd/storage_cost/storage_cost_params.h"

#include <variant>

#include "hogd/base/types.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

int128_t
StorageCostParams::BytestringCostParams::get_cost(
    StorageCostModel const &model) const
{
  return model.kBaseItemCost + model.get_bytestring_cost(bytestring_len);
}

int128_t
StorageCostParams::RetrocausalQueueCostParams::get_cost(
    StorageCostModel const &model) const
{
  return model.kBaseItemCost +
         model.get_retrocausal_queue_cost(active_queue_size,
                                          active_block_count);
}

int128_t
StorageCostParams::NNIntCostParams::get_cost(StorageCostModel const &model)
{
  return model.kBaseItemCost;
}

int128_t
StorageCostParams::get_cost(StorageCostModel const &model) const
{
  return std::visit([&model](
                        auto const &params) { return params.get_cost(model); },
                    params_);
}

} // namespace hogd
