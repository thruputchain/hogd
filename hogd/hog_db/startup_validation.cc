#include "hogd/hog_db/startup_validation.h"

#include "hogd/base/log.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"

namespace hogd {

bool
StartupValidation::tip_is_servable(HogDbDAO &db, const DbTip &tip,
                                   const StorageCostModel &cost_model)
{
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
  return true;
}

} // namespace hogd
