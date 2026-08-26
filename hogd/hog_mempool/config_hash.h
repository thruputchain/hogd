#pragma once

#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

struct StorageCostModel;

/// Build the consensus-critical post-block configuration preimage.
ConfigHashPreimage
make_config_hash_preimage(StorageCostModel const &storage_cost_model);

/// Compute the configuration commitment carried by ProposalHeader.
ConfigHash compute_config_hash(ConfigHashPreimage const &preimage);

/// Compute the current configuration commitment from its storage-cost member.
ConfigHash compute_config_hash(StorageCostModelXdr const &storage_cost_model);

} // namespace hogd
