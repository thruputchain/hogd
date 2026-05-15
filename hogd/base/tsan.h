// tsan.h - ThreadSanitizer mutex annotation macros for custom locks.
//
// TSAN's per-atomic-op tracking is in principle enough to derive
// happens-before through a lock built on acquire/release atomics, but
// in practice the derivation is fragile for custom rwlock patterns:
// reader-reader concurrency has no atomic-level encoding, release
// sequences through RMW chains are tracked conservatively, and spin
// loops with relaxed loads confuse the per-op model. The explicit
// __tsan_mutex_* annotations switch TSAN to its mutex-aware model where
// the bracketed region is treated as a single acquire/release with full
// rwlock semantics.
//
// Usage from a custom lock impl:
//
//   #include "hogd/base/tsan.h"
//
//   void lock_writer() {
//     HOG_TSAN_MUTEX_PRE_LOCK(this, /*flags=*/0);
//     ...cas spin...
//     HOG_TSAN_MUTEX_POST_LOCK(this, /*flags=*/0, /*recursion=*/1);
//   }
//
//   void lock_reader() {
//     HOG_TSAN_MUTEX_PRE_LOCK(this, HOG_TSAN_FLAG_READ);
//     ...cas spin...
//     HOG_TSAN_MUTEX_POST_LOCK(this, HOG_TSAN_FLAG_READ, 1);
//   }
//
//   void unlock_writer() {
//     HOG_TSAN_MUTEX_PRE_UNLOCK(this, /*flags=*/0);
//     ...release store...
//     HOG_TSAN_MUTEX_POST_UNLOCK(this, /*flags=*/0);
//   }
//
// For try-lock paths, OR HOG_TSAN_FLAG_TRY into pre/post-lock flags, and
// also OR HOG_TSAN_FLAG_TRY_FAILED into the post-lock flags on failure
// (with recursion=0).
//
// For reincarnated locks (e.g. bump-allocator reuse), call
// HOG_TSAN_MUTEX_DESTROY(this) followed by HOG_TSAN_MUTEX_CREATE(this).
//
// All macros compile to no-ops outside TSAN builds; the no-op forms
// cast their arguments to void so they don't produce unused-variable
// warnings when the caller computes flags into a local.

#pragma once

#if defined(__SANITIZE_THREAD__) ||                                            \
    (defined(__has_feature) && __has_feature(thread_sanitizer))

#include <sanitizer/tsan_interface.h>

#define HOG_TSAN_MUTEX_CREATE(addr) __tsan_mutex_create((addr), 0)
#define HOG_TSAN_MUTEX_DESTROY(addr) __tsan_mutex_destroy((addr), 0)
#define HOG_TSAN_MUTEX_PRE_LOCK(addr, flags)                                   \
  __tsan_mutex_pre_lock((addr), (flags))
#define HOG_TSAN_MUTEX_POST_LOCK(addr, flags, recursion)                       \
  __tsan_mutex_post_lock((addr), (flags), (recursion))
#define HOG_TSAN_MUTEX_PRE_UNLOCK(addr, flags)                                 \
  ((void)__tsan_mutex_pre_unlock((addr), (flags)))
#define HOG_TSAN_MUTEX_POST_UNLOCK(addr, flags)                                \
  __tsan_mutex_post_unlock((addr), (flags))

#define HOG_TSAN_FLAG_READ __tsan_mutex_read_lock
#define HOG_TSAN_FLAG_TRY __tsan_mutex_try_lock
#define HOG_TSAN_FLAG_TRY_FAILED __tsan_mutex_try_lock_failed

#else // TSAN not enabled

// No-op forms. The (void) casts on each argument ensure that locals
// callers compute (e.g. `unsigned f = HOG_TSAN_FLAG_READ;`) aren't
// flagged as unused.
#define HOG_TSAN_MUTEX_CREATE(addr) ((void)(addr))
#define HOG_TSAN_MUTEX_DESTROY(addr) ((void)(addr))
#define HOG_TSAN_MUTEX_PRE_LOCK(addr, flags) ((void)(addr), (void)(flags))
#define HOG_TSAN_MUTEX_POST_LOCK(addr, flags, recursion)                       \
  ((void)(addr), (void)(flags), (void)(recursion))
#define HOG_TSAN_MUTEX_PRE_UNLOCK(addr, flags) ((void)(addr), (void)(flags))
#define HOG_TSAN_MUTEX_POST_UNLOCK(addr, flags) ((void)(addr), (void)(flags))

#define HOG_TSAN_FLAG_READ 0u
#define HOG_TSAN_FLAG_TRY 0u
#define HOG_TSAN_FLAG_TRY_FAILED 0u

#endif
