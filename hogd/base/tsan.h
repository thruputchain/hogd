// tsan.h - ThreadSanitizer annotation macros for custom synchronization.
//
// TSAN's per-atomic-op tracking is in principle enough to derive
// happens-before through a lock built on acquire/release atomics, but
// in practice the derivation is fragile for custom rwlock patterns:
// reader-reader concurrency has no atomic-level encoding, release
// sequences through RMW chains are tracked conservatively, and spin
// loops with relaxed loads confuse the per-op model.
//
// These helpers add an explicit happens-before edge without making TSAN track
// the object as a mutex. That matters for locks embedded in graph-like data
// structures, where normal operation may acquire different node locks in
// orders that would look like lock-order inversions to TSAN's deadlock checker.
//
// Usage from a custom lock implementation:
//
//   #include "hogd/base/tsan.h"
//
//   void lock_writer() {
//     ...cas spin...
//     HOG_TSAN_ACQUIRE(this);
//   }
//
//   void unlock_writer() {
//     HOG_TSAN_RELEASE(this);
//     ...release store...
//   }
//
// All macros compile to no-ops outside TSAN builds.

#pragma once

#if defined(__SANITIZE_THREAD__) ||                                            \
    (defined(__has_feature) && __has_feature(thread_sanitizer))

#include <sanitizer/tsan_interface.h>

#define HOG_TSAN_ACQUIRE(addr) __tsan_acquire((addr))
#define HOG_TSAN_RELEASE(addr) __tsan_release((addr))

#else // TSAN not enabled

#define HOG_TSAN_ACQUIRE(addr) ((void)(addr))
#define HOG_TSAN_RELEASE(addr) ((void)(addr))

#endif
