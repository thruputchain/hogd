#include "hogd/hog_db/startup_validation.h"

#include "contract.hh"
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
  // default_wallet.contract_type XDR-decodes by cast and the tip row is
  // excluded from the state hash, so a tampered type reaches here with its
  // config hash recomputed. Reject a type outside the registered runtime set
  // before the root fallback serves it and dispatch reports Unrecoverable
  // (#4293); deployed-address metadata is guarded the same way on read.
  bool registered_wallet_type = false;
  switch (tip.default_wallet.contract_type) {
  case ContractType::kNative:
  case ContractType::kMockTest:
  case ContractType::kRv32:
    registered_wallet_type = true;
    break;
  }
  if (!registered_wallet_type) {
    critical("tip default wallet has an unregistered contract type {}",
             tip.default_wallet.contract_type);
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
