#include "hogd/hog_db/startup_validation.h"

#include <expected>
#include <string_view>

#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"

namespace hogd {

std::expected<void, TipRejection>
validate_recovered_tip(HogDbDAO &db, const DbTip &tip,
                       const StorageCostModel &cost_model)
{
  // Ahead of make_config_hash_preimage, which CHECKs this: a tip read back off
  // disk is untrusted the same way a peer's genesis is, so a non-canonical
  // list has to be a rejection rather than an abort.
  if (!config_hashable(tip.network_config)) {
    return std::unexpected(TipRejection::kNetworkConfigNotCanonical);
  }
  if (compute_config_hash(make_config_hash_preimage(cost_model,
                                                    tip.network_config,
                                                    tip.default_wallet)) !=
      tip.value.proposal_header.config_hash) {
    return std::unexpected(TipRejection::kConfigHashMismatch);
  }
  if (!db.read_contract_code(tip.default_wallet.hash)) {
    return std::unexpected(TipRejection::kDefaultWalletMissing);
  }
  return {};
}

std::string_view
describe_tip_rejection(TipRejection rejection)
{
  // No default arm: a new rejection has to be named here or -Wswitch says so.
  switch (rejection) {
  case TipRejection::kNetworkConfigNotCanonical:
    return "the network config parameters in the tip are not the gapless "
           "prefix 0, 1, 2, ... of the parameter ids";
  case TipRejection::kConfigHashMismatch:
    return "the chain configuration in the tip does not hash to the config "
           "hash its own committed header states";
  case TipRejection::kDefaultWalletMissing:
    return "no contract is stored under the default wallet hash the tip "
           "selects";
  }
  return "unknown tip rejection";
}

} // namespace hogd
