#pragma once

#include <atomic>
#include <cstddef>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogdb/invariant.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

class HogConservationInvariant : public HogdbInvariant {
public:
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

  std::atomic<int128_t> total_supply_change_{0};
};

} // namespace hogd
