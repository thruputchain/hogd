// startup_validation.h - gating a recovered database before it serves anything.

#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"

namespace hogd {

// What made a recovered tip unservable.
enum class TipRejection : uint8_t {
  // The tip's network config is not the gapless prefix of parameter ids that
  // a hashable configuration is, so it has no config hash to be checked
  // against.
  kNetworkConfigNotCanonical,

  // The chain configuration the tip carries does not hash to the config hash
  // the tip's own committed header states.
  kConfigHashMismatch,

  // Nothing is stored under the contract hash the tip selects as its default
  // wallet.
  kDefaultWalletMissing,
};

// Whether a recovered `tip` may be served, given the cost model stored
// alongside it.
//
// The chain configuration a node executes under -- cost model, network config,
// default wallet -- reaches it through `<DBTIP` and `<COSTMODEL`, which are
// global metadata: is_state_item() leaves them out of compute_full_db_hash(),
// so a tip whose storage_long_hash matches the recomputed state hash has said
// nothing about any of them. ProposalHeader::config_hash has: consensus agreed
// it, and every block recomputes it. Re-deriving it here is what ties the
// configuration this node is about to execute under to the one its peers
// committed, rather than to whatever its data directory happens to hold
// (#3926).
//
// Also refuses a default wallet with no contract row. That one resolves during
// execution rather than at lookup, so leaving it to be found later turns a
// root call to an undeployed address into SystemErrorCode::Unrecoverable.
//
// Takes the tip's configuration to be the one its own header commits to, which
// holds while ConsensusValue::upgrades is unimplemented. A config upgrade
// applied during a commit would leave the post-upgrade configuration in the
// tip against a header naming the pre-upgrade hash, so this has to learn the
// upgrade rule when that lands.
std::expected<void, TipRejection>
validate_recovered_tip(HogDbDAO &db, const DbTip &tip,
                       const StorageCostModel &cost_model);

// What `rejection` found, for the startup log.
std::string_view describe_tip_rejection(TipRejection rejection);

} // namespace hogd
