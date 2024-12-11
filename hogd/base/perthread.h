// perthread.h - utilities for per-thread variables.
//
// This library provides a variant of thread_local called PerThread. Unlike
// thread_local, it supports per-thread variables that are local rather than
// global in scope.  For example, if a class uses PerThread to define a member
// variable, every instance will have its own per-thread copies of the
// variable.
//
// The implementation strategy is to maintain a thread_local std::vector<T> for
// each T. Then, each PerThread instance refers to an index in the
// std::vector<T>. A per-type lock is use to synchronize when threads are
// spawning and exiting and to adjust the size of the std::vector<T> when more
// indices are needed. Threads allocate memory for their index lazily (on the
// first access) to avoid wasting memory if the thread never touches the
// PerThread variable.
//
// See also alloc_percpu() in the Linux Kernel, which works in a similar way
// but uses a per-core heap that stores a mix of types in one mapping. We don't
// do this here in order to avoid the implementation complexity of a full heap
// allocator, and the added difficulty of constructing and destructing types
// each time a new thread spawns and exits. Instead we went with a per-thread
// std::vector<T> for each T, so that each type is grouped together, making
// it easy to know how to construct and destruct items.

#pragma once

#include <concepts>
#include <mutex>
#include <utility>
#include <vector>

#include "hogd/base/types.h"

namespace hogd {

namespace detail {

template<typename T> class TLStorage {
public:
  // alloc_slot assigns an index in the table of thread-local values.
  static size_t alloc_slot()
  {
    std::scoped_lock g(lock_);
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i])
        continue;
      slots_[i] = true;
      return i;
    }
    slots_.push_back(true);
    return slots_.size() - 1;
  }

  // free_slot releases an index in the table of thread-local values.
  static void free_slot(size_t idx)
  {
    std::scoped_lock g(lock_);
    for_each(idx, [](T &item) {
      std::destroy_at(&item);
      std::construct_at(&item);
    });
    slots_[idx] = false;
  }

  // populate ensures that the item has been allocated an initialized.
  static void populate(size_t idx)
  {
    if (items_.size() <= idx) [[unlikely]]
      expand(idx + 1);
  }

  // get retrieves a thread-local value for a given index. It creates the item
  // if it hasn't yet been initialized.
  static T *get(size_t idx)
  {
    populate(idx);
    return &items_[idx];
  }

  // for_each runs func on each thread-local value for a given index. lock_
  // must be held before calling this function. This allows the caller to
  // synchronize access to other variables while preventing the set of threads
  // from changing.
  template<typename F> static void for_each(size_t idx, F func)
  {
    for (TLItems *items : threads_) {
      if (items->size() > idx)
        func((*items)[idx]);
    }
  }

  static inline std::mutex lock_;

private:
  struct TLItems : public std::vector<T> {
    ~TLItems() { TLStorage::remove(this); }
  };

  static inline thread_local TLItems items_;
  static inline std::vector<TLItems *> threads_;
  // TODO(amb): use a dynamic bitset here.
  static inline std::vector<bool> slots_;

  static void expand(size_t n)
  {
    std::scoped_lock g(lock_);
    if (items_.empty())
      threads_.push_back(&items_);
    items_.resize(n);
  }

  static void remove(TLItems *items)
  {
    if (items->empty())
      return;

    std::scoped_lock g(lock_);
    items->clear();
    for (size_t i = 0; i < threads_.size(); ++i) {
      if (threads_[i] != items)
        continue;

      threads_[i] = threads_[threads_.size() - 1];
      try {
        threads_.resize(threads_.size() - 1);
      } catch (const std::exception &e) {
        threads_[i] = nullptr;
      }
      break;
    }
  }
};

} // namespace detail

// PerThread is a thread-local variable that is not global (like thread_local
// would be). Instead, it behaves more like a normal local variable, except
// each thread gets its own per-thread copy.
template<typename T> class PerThread {
public:
  PerThread() : idx_(storage_.alloc_slot()) {}
  ~PerThread() { storage_.free_slot(idx_); }

  // Enable move and disable copy.
  PerThread(PerThread &&pt) noexcept : idx_(pt.idx_) {}
  PerThread &operator=(PerThread &&pt) noexcept
  {
    idx_ = pt.idx_;
    return *this;
  }
  PerThread(const PerThread &pt) = delete;
  PerThread &operator=(const PerThread &pt) = delete;

  T *operator->() const { return storage_.get(idx_); }

  typename std::add_lvalue_reference_t<T> operator*() const
  {
    return *storage_.get(idx_);
  }

  // populate ensure that the item has been allocated and initialized. This is
  // only necessary if you want to maintain references to more than one
  // PerThread of the same type. It prevents dereferencing from causing
  // allocations, which would invalidate any previous references.
  void populate() const { return storage_.populate(idx_); }

  // lock_threads returns an acquired lock that prevents threads from leaving
  // or joining. It is usually used in combination with for_each(), and allows
  // for extra synchronization of any shared state.
  std::unique_lock<std::mutex> lock_threads()
  {
    return std::unique_lock(storage_.lock_);
  }

  // for_each runs func on each thread-local copy of the variable. The lock
  // returned by lock_threads() must be passed as ul.
  template<typename F> void for_each(std::unique_lock<std::mutex> &ul, F &&func)
  {
    assert(ul.owns_lock());
    assert(ul.mutex() == &storage_.lock_);
    storage_.for_each(idx_, std::forward<F>(func));
  }

private:
  static inline detail::TLStorage<T> storage_;
  size_t idx_;
};

// PerThreadCounter is a scalable, precise counter that exploits TLS to avoid
// coherence traffic. Incrementing the counter is cheap while getting its value
// is expensive.
template<std::integral T> class PerThreadCounter {
public:
  PerThreadCounter() : global_counter_(0) {}
  PerThreadCounter(T val) : global_counter_(val) {}

  // get returns the current count.
  T get()
  {
    std::unique_lock ul = tl_counters_.lock_threads();
    T cnt = global_counter_;
    tl_counters_.for_each(ul,
                          [&cnt](const TLCounter &val) { cnt += val.count; });
    return cnt;
  }

  // inc increments the count by val.
  void inc(T val = 1)
  {
    TLCounter &tl_count = *tl_counters_;
    tl_count.count += val;
    if (!tl_count.global_count) [[unlikely]]
      tl_count.global_count = &global_counter_;
  }

private:
  struct TLCounter {
    T count{0};
    T *global_count{nullptr};

    TLCounter() = default;
    // The threads lock is held when this destructor is called, synchronizing
    // access to global_counter_.
    ~TLCounter()
    {
      if (global_count)
        *global_count += count;
    }
  };

  PerThread<TLCounter> tl_counters_;
  T global_counter_;
};

} // namespace hogd
