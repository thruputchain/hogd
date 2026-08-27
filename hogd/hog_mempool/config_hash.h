#pragma once

#include "header.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

inline ConfigHashPreimage
make_config_hash_preimage(const StorageCostModel &storage_cost_model)
{
  return ConfigHashPreimage{
      .storage_cost_model = storage_cost_model.to_xdr(),
  };
}

inline ConfigHash
compute_config_hash(const ConfigHashPreimage &preimage)
{
  return sha256(preimage);
}

inline ConfigHash
compute_config_hash(const StorageCostModelXdr &storage_cost_model)
{
  return compute_config_hash(
      ConfigHashPreimage{.storage_cost_model = storage_cost_model});
}

} // namespace hogd
