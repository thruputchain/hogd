#pragma once

#include "contract.hh"
#include "genesis.hh"
#include "header.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/crypto/crypto.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

// Whether `network_config` may be hashed at all. Every path taking one from an
// untrusted peer must check this first; the hash functions CHECK it.
inline bool
config_hashable(const NetworkConfig &network_config)
{
  return NetworkConfigParams::is_canonical(network_config.v0());
}

inline ConfigHashPreimage
make_config_hash_preimage(const StorageCostModel &storage_cost_model,
                          const NetworkConfig &network_config,
                          const DeployedContractHash &default_wallet)
{
  // Callers refuse a non-canonical configuration at ingress, so one reaching
  // here is this node being inconsistent rather than untrusted input.
  CHECK(config_hashable(network_config));
  return ConfigHashPreimage{
      .storage_cost_model = storage_cost_model.to_xdr(),
      .network_config = network_config,
      .default_wallet = default_wallet,
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
  CHECK(config_hashable(genesis_configuration.network_config));
  return compute_config_hash(ConfigHashPreimage{
      .storage_cost_model = genesis_configuration.storage_cost_model,
      .network_config = genesis_configuration.network_config,
      .default_wallet = genesis_configuration.default_wallet,
  });
}

} // namespace hogd
