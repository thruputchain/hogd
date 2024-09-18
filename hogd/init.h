// init.h - initialization

#pragma once

#include "hogd/base/log.h"
#include "hogd/base/status.h"

namespace hogd {

// must be called before using logging
extern void log_init(std::unique_ptr<Logger> logger);

// InitFunc is the type for each initialization function
using InitFunc = GStatus<void> (*)();

// List your initialization functions here
extern GStatus<void> crypto_init();

// hogd_init must be called first before using code in hogd.
//
// Call this in your main() function.
//
// Returns 0 if successful, otherwise failure.
extern int hogd_init(int argc, char *argv[]);

} // namespace hogd
