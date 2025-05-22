// -*- C++ -*-

#pragma once

#include <atomic>
#include <memory>

namespace x23 {

#if HAVE_ATOMIC_SHARED_PTR

template<typename T> using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;

#else // !HAVE_ATOMIC_SHARED_PTR

// NOLINTNEXTLINE(readability-identifier-naming)
template<typename T> struct atomic_shared_ptr : std::shared_ptr<T> {
  using super = std::shared_ptr<T>;

  using super::super;
  // Because inherited copy/move constructors don't count
  atomic_shared_ptr(const super &other) : super(other) {}
  atomic_shared_ptr(super &&other) : super(std::move(other)) {}
  // Because you can't copy/move std::atomic
  atomic_shared_ptr(const atomic_shared_ptr &) = delete;

  // Because std::atomic<std::shared_ptr<T>>::operator= also returns void
  void operator=(std::nullptr_t) { store(nullptr); }
  void operator=(super desired) { store(std::move(desired)); }

  super load() const { return std::atomic_load(this); }
  void store(super desired) { std::atomic_store(this, std::move(desired)); }
  super exchange(super desired)
  {
    return std::atomic_exchange(this, std::move(desired));
  }
  bool compare_exchange_strong(super &expected, super desired)
  {
    return std::atomic_compare_exchange_strong(this, &expected,
                                               std::move(desired));
  }
};

#endif // !HAVE_ATOMIC_SHARED_PTR

} // namespace x23
