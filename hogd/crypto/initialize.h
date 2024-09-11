#pragma once

#include "hogd/crypto/crypto_error.h"

namespace hogd {

/** This function has to be called before
 * any functions within libsodium are called.
 *
 * This is threadsafe and designed to be cheap to call.
 */
CryptoStatus<void> initialize_crypto();

} // namespace hogd
