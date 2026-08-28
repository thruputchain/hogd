#pragma once

#include "genesis.hh"
#include "header.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

inline ConfigHashPreimage
make_config_hash_preimage(const StorageCostModel &storage_cost_model,
                          const NetworkConfig &network_config)
{
  return ConfigHashPreimage{
      .storage_cost_model = storage_cost_model.to_xdr(),
      .network_config = network_config,
  };
}

inline ConfigHash
compute_config_hash(const ConfigHashPreimage &preimage)
{
  return sha256(preimage);
}

/// The config hash a chain's genesis block commits to.
///
/// Takes the whole configuration rather than its parts so a new consensus-
/// critical field added to GenesisConfiguration cannot be left out of the
/// hash by a caller that was not updated.
inline ConfigHash
compute_config_hash(const GenesisConfiguration &genesis_configuration)
{
  return compute_config_hash(ConfigHashPreimage{
      .storage_cost_model = genesis_configuration.storage_cost_model,
      .network_config = genesis_configuration.network_config,
  });
}

} // namespace hogd
