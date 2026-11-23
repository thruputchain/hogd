#pragma once

#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/status.h"
#include "types.hh"

namespace hogd {

// Makes every address's native-token age row hold exactly its balance, before
// the mempool scheduler reads any row. [recovered_height] is the recovered
// committed tip's height, so it must be nonnegative.
HogDbStatus<void>
reconcile_startup_native_token_ages(HogDbDAO &db, BlockHeight recovered_height);

} // namespace hogd
