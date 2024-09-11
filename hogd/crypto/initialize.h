#pragma once

#include "hogd/base/status.h"

namespace hogd {

/**
 * This function has to be called before
 * any functions within libsodium are called.
 *
 * This is threadsafe and designed to be cheap to call.
 */
GStatus<void> initialize_crypto();

} // namespace hogd
