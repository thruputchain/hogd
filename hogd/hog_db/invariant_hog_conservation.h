#pragma once

#include <cstddef>
#include <span>

#include "hogd/base/types.h"
#include "hogd/base/wide_integral.h"
#include "hogd/hog_db/invariant.h"
#include "hogd/hog_db/status.h"
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
                       HogDbStatus<DBItem> const &before,
                       HogDbStatus<DBItem> const &after) override;

private:
  void handle_balance_change(HogDbStatus<DBItem> const &before,
                             HogDbStatus<DBItem> const &after);
  void handle_contract_data_item_change(std::span<const std::byte> key,
                                        HogDbStatus<DBItem> const &before,
                                        HogDbStatus<DBItem> const &after);
  void handle_address_metadata_item_change(HogDbStatus<DBItem> const &before,
                                           HogDbStatus<DBItem> const &after);
  void handle_address_auth_data_item_change(HogDbStatus<DBItem> const &before,
                                            HogDbStatus<DBItem> const &after);
  void handle_replay_cache_item_change(std::span<const std::byte> key,
                                       HogDbStatus<DBItem> const &before,
                                       HogDbStatus<DBItem> const &after);

  void update_total_supply(int128_t change);

  // Running sum of every HOG-denominated item change in the block. Only the
  // final value has to be zero: partial sums legitimately leave int128 range
  // whenever a single item moves an amount near INT128_MAX (minting the whole
  // supply credits a balance by INT128_MAX and debits the total-supply counter
  // by the same, and the two arrive as separate changes), so the accumulator is
  // 256 bits wide to keep those intermediates from overflowing.
  x23::MVar<WideIntegral<int128_t>> total_supply_change_{0};
  StorageCostModel cost_model_;
};

} // namespace hogd
