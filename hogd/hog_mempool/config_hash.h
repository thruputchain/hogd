#pragma once

#include "genesis.hh"
#include "header.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/crypto/crypto.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

/// Whether `network_config` is in the only form that may be hashed.
///
/// A v1 configuration states its parameters as a list, and that list is hashed
/// as written. Two orderings of one set of parameters, or a repeat, would give
/// a configuration more than one hash -- so nodes agreeing on a chain's rules
/// could still disagree on its config hash. Only a strictly ascending, and so
/// duplicate-free, list is hashable.
inline bool
config_hashable(const NetworkConfig &network_config)
{
  return network_config.v() != 1 ||
         config_params_canonical(network_config.v1());
}

inline ConfigHashPreimage
make_config_hash_preimage(const StorageCostModel &storage_cost_model,
                          const NetworkConfig &network_config)
{
  // A non-canonical configuration must be refused where it enters -- genesis
  // load does that -- so reaching here with one is an internal inconsistency,
  // not untrusted input.
  CHECK(config_hashable(network_config));
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
  CHECK(config_hashable(genesis_configuration.network_config));
  return compute_config_hash(ConfigHashPreimage{
      .storage_cost_model = genesis_configuration.storage_cost_model,
      .network_config = genesis_configuration.network_config,
  });
}

} // namespace hogd
