#include "hogd/hog_db/startup_validation.h"

#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"

namespace hogd {

bool
StartupValidation::tip_is_servable(HogDbDAO &db, const DbTip &tip,
                                   const StorageCostModel &cost_model)
{
  // The -1 pre-genesis sentinel is only synthesized for a database with no
  // persisted tip. Accepting it here reopens the genesis admission path.
  if (tip.value.proposal_header.block_height < 0) {
    critical("recovered tip has a negative committed block height");
    return false;
  }
  // Ahead of make_config_hash_preimage, which CHECKs this.
  if (!config_hashable(tip.network_config)) {
    critical("tip network config is not a gapless prefix of parameter ids");
    return false;
  }
  if (compute_config_hash(make_config_hash_preimage(cost_model,
                                                    tip.network_config,
                                                    tip.default_wallet)) !=
      tip.value.proposal_header.config_hash) {
    critical("tip configuration does not hash to its committed config hash");
    return false;
  }
  // Height 0 is exempt: a chain may name a wallet its genesis ceremony
  // deploys in the height-1 block.
  if (tip.value.proposal_header.block_height > 0 &&
      !db.read_contract_code(tip.default_wallet.hash)) {
    critical("no contract stored for the tip's default wallet");
    return false;
  }
  // Genesis admission enforces this savepoint-depth domain, but the config hash
  // above only binds the config to consensus; it does not range-check it. A
  // recovered zero depth is accepted by every gate above yet leaves the first
  // root push (which must create one savepoint) returning kRPCUnrecoverable,
  // and a depth past the ceiling exceeds what this build's executor stacks
  // sustain. Reapply the genesis floor and ceiling here (#4181).
  const NetworkConfigParams config_params(tip.network_config.v0());
  if (config_params.max_savepoint_depth == 0) {
    critical("recovered tip max_savepoint_depth must be at least 1 for root "
             "commits");
    return false;
  }
  if (config_params.max_savepoint_depth > kSavepointDepthCeiling) {
    critical("recovered tip max_savepoint_depth {} exceeds the {} this build's "
             "executor stacks sustain",
             config_params.max_savepoint_depth, kSavepointDepthCeiling);
    return false;
  }
  return true;
}

} // namespace hogd
