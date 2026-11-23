#pragma once

#include "hogd/base/types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/status.h"

namespace hogd {

// Makes every address's native-token age row hold exactly its balance, before
// the mempool scheduler reads any row. The scheduler writes the rows between
// block commits, and a write becomes durable only with the next commit's sync,
// so after a restart a row can trail the balances committed at
// [recovered_height]. Tokens the row is missing are credited as received at
// [recovered_height]; tokens it has too many of are drained from the received
// group first.
//
// [recovered_height] is the height of the recovered committed tip, so it must
// be nonnegative: a database with no committed tip has nothing to reconcile.
HogDbStatus<void>
reconcile_startup_native_token_ages(HogDbDAO &db, BlockHeight recovered_height);

} // namespace hogd
