#pragma once

#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
reconcile_startup_hog_ages(HogDBDAO &db);

} // namespace hogd
