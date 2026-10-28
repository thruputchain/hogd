// startup_validation.h - gating a recovered database before it serves anything.

#pragma once

#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"

namespace hogd {

class StartupValidation {
public:
  // Whether a recovered `tip` may be served, logging what failed if not. The
  // `<DBTIP` and `<COSTMODEL` rows are global metadata, outside the consensus
  // state hash, so re-deriving the config hash here is the only thing binding
  // the configuration a node executes under to the config hash it proposes
  // and validates against (#3926). This checks that the tip is
  // self-consistent and runnable, not that it is authentic: whoever can write
  // the row can also rewrite `storage_long_hash`, so a valid but different
  // configuration passes, and peers reject the resulting proposals (#4695).
  static bool tip_is_servable(HogDbDAO &db, const DbTip &tip,
                              const StorageCostModel &cost_model);
};

} // namespace hogd
