#pragma once

#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/status.h"

namespace hogd {

// [committed_height] is the recovered committed tip's height (-1 for a fresh
// database); no valid average age exceeds it.
HogDbStatus<void>
reconcile_startup_native_token_ages(HogDbDAO &db, BlockHeight committed_height);

} // namespace hogd
