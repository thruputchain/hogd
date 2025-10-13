#pragma once

#include <cstddef>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogdb/invariant.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/mvar.h"

namespace hogd {

class HogConservationInvariant : public HogdbInvariant {
public:
  HogConservationInvariant(StorageCostModel const &cost_model)
    : cost_model_(cost_model)
  {}
  bool check() override;
  void log_item_change(std::span<const std::byte> key,
                       HogDBStatus<DBItem> const &before,
                       HogDBStatus<DBItem> const &after) override;

private:
  void handle_balance_change(HogDBStatus<DBItem> const &before,
                             HogDBStatus<DBItem> const &after);
  void handle_contract_data_item_change(HogDBStatus<DBItem> const &before,
                                        HogDBStatus<DBItem> const &after);
  void handle_address_metadata_item_change(HogDBStatus<DBItem> const &before,
                                           HogDBStatus<DBItem> const &after);
  void handle_account_auth_data_item_change(HogDBStatus<DBItem> const &before,
                                            HogDBStatus<DBItem> const &after);
  void handle_replay_cache_item_change(HogDBStatus<DBItem> const &before,
                                       HogDBStatus<DBItem> const &after);

  void update_total_supply(int128_t change);

  x23::MVar<int128_t> total_supply_change_{0};
  StorageCostModel cost_model_;
};

} // namespace hogd
