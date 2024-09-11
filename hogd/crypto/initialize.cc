#include "hogd/crypto/initialize.h"

#include <sodium.h> // IWYU pragma: keep

#include <atomic>

#include "hogd/base/compiler.h"
#include "hogd/crypto/crypto_error.h"

namespace hogd {

std::atomic<bool> initialized_flag = false;

CryptoStatus<void>
initialize_crypto()
{
  // It's safe to call sodium_init concurrently.
  // But on all non-wasm platforms (where wasm is singlethreaded, at least at
  // the moment) this acquires a writelock (which is unnecessary). Relaxed
  // atomics because sodium internally takes care of everything important
  // regarding synchronization
  if (likely(initialized_flag.load(std::memory_order_relaxed))) {
    return {};
  }

  // This function has to be called to completion before ANY call to ANY sodium
  // method begins

  // NOLINTBEGIN(misc-include-cleaner)
  if (sodium_init() == -1) {
    // failed to initialize libsodium
    return CryptoError{CryptoErrorCode::kInitializeFailure};
  }
  // NOLINTEND(misc-include-cleaner)

  initialized_flag.store(true, std::memory_order_relaxed);

  return {};
}

} // namespace hogd
