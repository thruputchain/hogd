#include "hogd/hog_db/startup_validation.h"

#include <limits>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

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
  // The successor height is block_height + 1, which must stay representable in
  // the signed BlockHeight. A live chain never reaches this height, so it only
  // appears on a tampered recovered tip (issue #4291).
  if (tip.value.proposal_header.block_height ==
      std::numeric_limits<BlockHeight>::max()) {
    critical("recovered tip is at the maximum committed block height");
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
  // The tip row is outside the state hash, so a CRC-valid, self-consistent
  // rewrite can set max_savepoint_depth to a value genesis would reject. Every
  // root commit pushes a savepoint, so a recovered zero bricks the first root
  // push with kRPCUnrecoverable; reapply the genesis bounds here (#4336).
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
  // The tip row is outside the state hash, so a tampered type can pass the
  // config hash (#4293); kMockTest is test-only, never a real default wallet.
  switch (tip.default_wallet.contract_type) {
  case ContractType::kNative:
  case ContractType::kRv32:
    break;
  default:
    error("tip default wallet has an unregistered contract type {}",
          tip.default_wallet.contract_type);
    return false;
  }
  // Height 0 is exempt: a chain may name a wallet its genesis ceremony
  // deploys in the height-1 block.
  if (tip.value.proposal_header.block_height > 0) {
    const auto code = db.read_contract_code(tip.default_wallet.hash);
    if (!code) {
      critical("no contract stored for the tip's default wallet");
      return false;
    }
    // The tip row is outside the state hash, so a tamper can keep the code hash
    // while rewriting the claimed type (#4306); bind it to the decoded row.
    Contract contract;
    try {
      x23::deserialize(&contract, x23::BufView{*code});
    } catch (const x23::XdrException &e) {
      critical("tip default wallet code row does not decode as a contract: {}",
               e.what());
      return false;
    }
    if (contract.contract_type() != tip.default_wallet.contract_type) {
      critical("tip default wallet code type {} does not match claimed type {}",
               contract.contract_type(), tip.default_wallet.contract_type);
      return false;
    }
  }
  return true;
}

} // namespace hogd
