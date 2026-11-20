#pragma once

#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/status.h"

namespace hogd {

HogDbStatus<void> reconcile_startup_native_token_ages(HogDbDAO &db);

} // namespace hogd
