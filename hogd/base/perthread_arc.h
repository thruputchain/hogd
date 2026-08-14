#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"

namespace hogd {

// PerThreadARC contains a refcounted value, where each refcount is stored in a
// threadlocal variable. In the normal case of operations, changing each
// refcount should be fast (each thread stores its refcount in a threadlocal
// counter).
//
// The value is not destroyed immediately when all references are released.
// Instead, initiate_shutdown() must be called to initiate the shutdown process.
// This aggregates all of the remaining refcounts into a single atomic variable.
// Once the shutdown process is initiated, no new references can be acquired,
// and changes to the refcount contend on a single shared variable (so refcount
// changes will be slower).
//
// release_reference() will return true if the value can be destroyed (i.e. no
// pending references). initiate_shutdown() will return true only if there are
// no outstanding references.
//
// Threadlocal refcounts are initialized to kUninitializedValue = 0.
// If a thread is to add its threadlocal count to the overall refcount,
// it must first acquire the threadpool lock (refcount_.lock_threads()).
// This should be unnecessary, except for the case where a new thread is created
// concurrently with the shutdown process.
// We could avoid this check if PerThread<T> had a separate workflow for
// creating new objects vs reinitializing existing objects.
//
template<typename T> class PerThreadARC {
public:
  template<typename... Args> requires std::constructible_from<T, Args...>
  PerThreadARC(Args &&...args) : value_(std::forward<Args>(args)...)
  {}

  T &get(this auto &self) { return self.value_; }

  PerThreadARC(PerThreadARC &&other) noexcept = delete;
  PerThreadARC &operator=(PerThreadARC &&other) noexcept = delete;
  PerThreadARC(const PerThreadARC &other) = delete;
  PerThreadARC &operator=(const PerThreadARC &other) = delete;

  // Returns true if a reference is acquired.  This can only fail if the value
  // is currently being destroyed.
  bool try_acquire_reference()
  {
    auto r = refcount_->count_.fetch_add(1, std::memory_order_relaxed);

    // If this is the first access: make sure that if were were in the middle of
    // shutdown, our thread's refcount is tracked by initiate_shutdown().
    if (r == kUninitializedValue) {
      auto ul = refcount_.lock_threads();

      if (is_shutdown_ready_) {
        // We're in shutdown mode, so we cannot acquire a reference.
        refcount_->count_.store(kShutdownValue, std::memory_order_relaxed);
        return false;
      }

      // We can acquire a reference, and we know that the next call to
      // initiate_shutdown() will track our thread's refcount.
      refcount_->count_.store(kInitializedValue + 1, std::memory_order_relaxed);
      refcount_->aggregate_ = &exited_thread_refcount_;
      return true;
    }

    // r > 0 if the refcount was near kInitializedValue, and r < 0 if the
    // refcount was near kShutdownValue.
    return r >= 0;
  }

  // Returns true if the value should be destroyed.
  bool release_reference() noexcept
  {
    // The fast path: Decrease the current threadlocal count by 1.
    // If the threadlocal count is still positive, then shutdown has not yet
    // been initiated.
    int64_t previous_threadlocal_count =
        refcount_->count_.fetch_sub(1, std::memory_order_release);

    // This thread did not touch the refcount before; so, we must initialize the
    // refcount and try again
    if (previous_threadlocal_count == kUninitializedValue) {
      auto ul = refcount_.lock_threads();

      if (is_shutdown_ready_) {
        refcount_->count_.store(kShutdownValue, std::memory_order_relaxed);
        // now reduce the active refcount by 1, and check if there are any
        // references left
        auto prev =
            shutdown_pending_refcount_.fetch_sub(1, std::memory_order_acq_rel);
        CHECK(prev > 0);
        return prev == 1;
      }

      // otherwise, initialize the refcount to -1; we're not in shutdown, so we
      // must return false.
      refcount_->count_.store(kInitializedValue - 1, std::memory_order_release);
      refcount_->aggregate_ = &exited_thread_refcount_;
      return false;
    }

    if (previous_threadlocal_count > 0) {
      return false;
    }
    // refcount_ was negative -- meaning shutdown was initiated.  We might still
    // be in the initiate_shutdown() for_each loop; so we wait for
    // is_shutdown_ready_ to be true.

    // We must be in shutdown mode
    // Ensure that the thread that called initiate_shutdown() has finished
    // iterating over all the threadlocal counts to produce the
    // shutdown_pending_refcount_
    is_shutdown_ready_.wait(false, std::memory_order_acquire);

    // Subtract the reference from the aggregated refcount
    auto prev =
        shutdown_pending_refcount_.fetch_sub(1, std::memory_order_acq_rel);
    CHECK(prev > 0);
    return prev == 1;
  }

  // Initiates shutdown of the value.
  // Returns true if the value can be destroyed immediately (no pending
  // references)
  // Caller is responsible for ensuring that before the object is actually
  // destroyed, other threads are eventually blocked from being able to try
  // to acquire a reference.
  bool initiate_shutdown()
  {
    // already being shutdown
    if (is_shutdown_ready_.load(std::memory_order_acquire)) {
      return false;
    }

    int64_t pending_count = 0;

    auto ul = refcount_.lock_threads();
    refcount_.for_each(ul, [&pending_count](Refcount &refcount) {
      int64_t prev_count = refcount.count_.exchange(kShutdownValue);

      // The value here could be: some value near kInitializedValue, or
      // kUninitializedValue +/- 1. We ignore the kUninitializedValue +/- 1
      // cases; in the +1 case, the try_acquire reference will fail; (by the
      // time that the thread on try_acquire_reference() acquires the thread
      // lock, is_shutdown_ready_ will be true). In the -1 case, the other
      // thread is responsible for rechecking shutdown_pending_refcount_.
      CHECK(prev_count >= 1000 || prev_count == kUninitializedValue ||
            prev_count == kUninitializedValue - 1 ||
            prev_count == kUninitializedValue + 1 ||
            prev_count == kInitializedValue - 1 ||
            prev_count == kInitializedValue + 1);

      if (prev_count == kUninitializedValue ||
          prev_count == kUninitializedValue + 1 ||
          prev_count == kUninitializedValue - 1) {
        // The thread is in the process of initializing its refcount.  We ignore
        // it here. The other thread is responsible for adding its refcount to
        // the shutdown_pending_refcount_ and checking is_shutdown_ready_.
        return;
      }

      pending_count += prev_count - kInitializedValue;
    });
    // Threads that already exited folded their deltas here (under this lock).
    pending_count += exited_thread_refcount_;
    CHECK(pending_count >= 0);

    // Retain a self-reference (the "+ 1") across readiness publication.
    // Publishing is_shutdown_ready_ lets a concurrent last releaser drain
    // shutdown_pending_refcount_ to zero and destroy this object; the extra
    // count keeps it alive until we have finished touching it below (notably
    // is_shutdown_ready_.notify_all()).
    shutdown_pending_refcount_.store(pending_count + 1,
                                     std::memory_order_relaxed);

    is_shutdown_ready_.store(true, std::memory_order_release);
    is_shutdown_ready_.notify_all();

    // Drop our self-reference. If all real references have already been
    // released (or there were none), we are the last owner and the caller may
    // destroy the object. Exactly one party observes the transition to zero,
    // so destruction ownership is unambiguous.
    auto prev =
        shutdown_pending_refcount_.fetch_sub(1, std::memory_order_acq_rel);
    CHECK(prev >= 1);
    return prev == 1;
  }

  ~PerThreadARC()
  {
    if (!is_shutdown_ready_.load(std::memory_order_acquire)) {
      [[maybe_unused]]
      auto r = initiate_shutdown();
      CHECK(r);
    }
    CHECK(shutdown_pending_refcount_ == 0);
  }

private:
  T value_;

  // Wrap atomic<int64_t> to force initialization to 0.
  // PerThread<T> requires movable types.
  struct Refcount {
    Refcount() : count_(kUninitializedValue) {}
    // Why is this ok?
    // The threadlocal vector inside PerThread<T> is only ever modified if the
    // PerThread lock is held. That's the lock returned by
    // PerThread<T>::lock_threads().
    //
    // This means that this function cannot ever be called concurrently with the
    // shutdown loop in initiate_shutdown().
    //
    // Additionally, this cannot be called concurrently with a fetch_add op in
    // try_acquire_reference() or release_reference(), as the get() call in
    // PerThread<T> does not return until after this function has finished.
    //
    // This guarantees that we cannot lose references to the underlying object.
    Refcount(Refcount &&obj) noexcept
    {
      count_ =
          obj.count_.exchange(kUninitializedValue, std::memory_order_relaxed);
      aggregate_ = obj.aggregate_;
      obj.aggregate_ = nullptr;
    }
    Refcount &operator=(Refcount &&obj) noexcept
    {
      count_ =
          obj.count_.exchange(kUninitializedValue, std::memory_order_relaxed);
      aggregate_ = obj.aggregate_;
      obj.aggregate_ = nullptr;
      return *this;
    }

    std::atomic<int64_t> count_;

    // &owner.exited_thread_refcount_, set on first touch; nullptr if untouched.
    int64_t *aggregate_ = nullptr;

    ~Refcount()
    {
      const int64_t c = count_.load(std::memory_order_relaxed);
      // A still-live slot (c > 0) means this thread is exiting before shutdown
      // drained it; fold its net contribution into the ARC aggregate instead of
      // losing the reference and aborting. Runs under TLStorage::lock_.
      if (c > 0 && aggregate_ != nullptr) {
        *aggregate_ += c - kInitializedValue;
        return;
      }
      CHECK(c <= 0);
    }
  };

  PerThread<Refcount> refcount_;
  std::atomic<bool> is_shutdown_ready_{false};
  std::atomic<int64_t> shutdown_pending_refcount_{0};

  // Net contribution of threads that touched this ARC then exited before
  // initiate_shutdown() drained their slots (see ~Refcount). Guarded by
  // refcount_'s thread lock -- fold sites and initiate_shutdown() all hold it
  // -- so it is a plain int, not atomic.
  int64_t exited_thread_refcount_{0};

  // too small to ever become nonnegative by +1 ops,
  // too big to every overflow to INT64_MAX by -1 ops
  constexpr static int64_t kShutdownValue = INT64_MIN / 2;
  // Too small to ever become nonnegative by +1 ops,
  // Too big to ever overflow to INT64_MAX by -1 ops
  constexpr static int64_t kInitializedValue = INT64_MAX / 2;
  // Too far away from both kInitializedValue and kShutdownValue
  // to ever be confused for a valid refcount.
  constexpr static int64_t kUninitializedValue = 0;
};

// RAII wrapper on PerThreadARC references.
// Used to make sure that the PerThreadARC reference is released and never
// forgotten. Does not trigger release_reference() in the destructor; instead,
// will debug assert if the reference is not released.
// (this is so that the release method can return the boolean regarding
// whether other garbage collection should be triggered.)

template<typename T> class PerThreadARCRef {
public:
  explicit PerThreadARCRef(PerThreadARC<T> *arc)
    : arc_(arc), active_(arc != nullptr)
  {}

  // copy is disabled
  PerThreadARCRef(const PerThreadARCRef &other) = delete;
  PerThreadARCRef &operator=(const PerThreadARCRef &other) = delete;

  // Move constructor
  PerThreadARCRef(PerThreadARCRef &&other) noexcept
    : arc_(other.arc_), active_(other.active_)
  {
    other.active_ = false;
  }

  // Move assignment
  PerThreadARCRef &operator=(PerThreadARCRef &&other) = delete;

  ~PerThreadARCRef() { assert(!active_); }

  // Pointer-like dereference operators
  T &operator*()
  {
    assert(arc_);
    return arc_->get();
  }
  const T &operator*() const
  {
    assert(arc_);
    return arc_->get();
  }
  T *operator->()
  {
    assert(arc_);
    return &arc_->get();
  }
  const T *operator->() const
  {
    assert(arc_);
    return &arc_->get();
  }

  operator bool() const { return arc_ != nullptr; }

  bool release_reference()
  {
    assert(arc_);
    active_ = false;
    return arc_->release_reference();
  }

private:
  PerThreadARC<T> *arc_;
  bool active_;
};

} // namespace hogd
