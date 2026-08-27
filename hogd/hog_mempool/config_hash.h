#pragma once

#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

struct StorageCostModel;

inline ConfigHashPreimage
make_config_hash_preimage(StorageCostModel const &storage_cost_model)
{
  return ConfigHashPreimage{
      .storage_cost_model = storage_cost_model.to_xdr(),
  };
}

inline ConfigHash
compute_config_hash(ConfigHashPreimage const &preimage)
{
  return sha256(preimage);
}

inline ConfigHash
compute_config_hash(StorageCostModelXdr const &storage_cost_model)
{
  return compute_config_hash(
      ConfigHashPreimage{.storage_cost_model = storage_cost_model});
}

} // namespace hogd
