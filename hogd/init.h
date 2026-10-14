// init.h - initialization

#pragma once

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"

namespace hogd {

// InitFunc is the type for each initialization function
using InitFunc = GStatus<void> (*)();

// List your initialization functions here
extern GStatus<void> base_init();
extern GStatus<void> crypto_init();

// hogd_init must be called first before using code in hogd.
//
// Call this in your main() function.
//
// Returns 0 if successful, otherwise failure.
extern int hogd_init(int argc, char *argv[], bool do_print_version = true,
                     LogLevel log_level = is_debug_build() ? LogLevel::kDebug
                                                           : LogLevel::kInfo);

// Prints "<program> <hogd-version>" and returns true when argv is exactly
// `--version`. Call before hogd_init() so version probes stay quiet and do not
// initialize subsystems.
extern bool print_version_and_exit_if_requested(int argc, char *argv[]);

} // namespace hogd
