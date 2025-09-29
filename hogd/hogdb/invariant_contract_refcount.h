#pragma once

#include <cstddef>
#include <span>
#include <unordered_map>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogdb/invariant.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

class HogContractRefcountInvariant : public HogdbInvariant {
public:
  bool check() override;
  void log_item_change(std::span<const std::byte> key,
                       HogDBStatus<DBItem> const &before,
                       HogDBStatus<DBItem> const &after) override;

private:
  void handle_wasm_refcount_change(std::span<const std::byte> key,
                                   HogDBStatus<DBItem> const &before,
                                   HogDBStatus<DBItem> const &after);
  void handle_address_metadata_item_change(HogDBStatus<DBItem> const &before,
                                           HogDBStatus<DBItem> const &after);

  void update_refcount(SHA256Hash const &hash, int32_t delta);

  // TODO(geoff): This is slow; if this is enabled outside of debug builds,
  // we need a faster implementation
  x23::MVar<std::unordered_map<SHA256Hash, int>> missing_contract_refcounts_;
};

} // namespace hogd
