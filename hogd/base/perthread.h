// perthread.h - utilities for per-thread variables.

#pragma once

#include <concepts>
#include <memory>
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
    for_each(idx, [](T &item) { item = T{}; });
    slots_[idx] = false;
  }

  // get retrieves a thread-local value for a given index. It creates the value
  // if it hasn't yet been initialized.
  static T *get(size_t idx)
  {
    if (items_.size() <= idx) [[unlikely]]
      expand(idx + 1);
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

  // for_each runs func each thread-local copy of the variable.
  template<typename F> void for_each(F &&func)
  {
    std::scoped_lock g(storage_.lock_);
    storage_.for_each(idx_, std::forward<F>(func));
  }

private:
  template<std::integral U> friend class PerThreadCounter;

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

  // get returns the current count
  T get() const
  {
    using storage = detail::TLStorage<TLCounter>;
    std::lock_guard g(storage::lock_);
    T cnt = global_counter_;
    storage::for_each(tl_counters_.idx_,
                      [&cnt](const TLCounter &val) { cnt += val.count; });
    return cnt;
  }

  // inc increments the count by val.
  void inc(T val = 1)
  {
    TLCounter &tl_count = *tl_counters_;
    tl_count.count += val;
    tl_count.global_count = &global_counter_;
  }

private:
  struct TLCounter {
    T count;
    T *global_count;

    // TLStorage's lock is held when this destructor is called, synchronizing
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
