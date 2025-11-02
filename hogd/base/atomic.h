#pragma once

#include <atomic>
#include <concepts>
#include <type_traits>

namespace hogd {

// GCC's stdlib does not provide integral specializations for
// std::atomic<int128_t>.

namespace detail {

template<typename T> struct GccAtomic16B : public std::atomic<T> {

  using std::atomic<T>::atomic;

  static_assert(
      !requires(std::atomic<T> arg) { arg.fetch_add(T{}); },
      "We should not be overriding an existing fetch_add method");

  T fetch_add(T arg,
              std::memory_order order = std::memory_order_seq_cst) noexcept
      requires std::integral<T>
  {
    for (;;) {
      T current = std::atomic<T>::load(std::memory_order_relaxed);
      T desired = current + arg;
      if (compare_exchange_weak(current, desired, order,
                                std::memory_order_relaxed)) {
        return current;
      }
    }
  }
  T fetch_sub(T arg,
              std::memory_order order = std::memory_order_seq_cst) noexcept
      requires std::integral<T>
  {
    for (;;) {
      T current = std::atomic<T>::load(std::memory_order_relaxed);
      T desired = current - arg;
      if (compare_exchange_weak(current, desired, order,
                                std::memory_order_relaxed)) {
        return current;
      }
    }
  }

  T operator+=(T arg) noexcept requires std::integral<T>
  {
    return fetch_add(arg) + 1;
  }
  T operator-=(T arg) noexcept requires std::integral<T>
  {
    return fetch_sub(arg) - 1;
  }

  T operator++(int) noexcept requires std::integral<T> { return fetch_add(1); }
  T operator--(int) noexcept requires std::integral<T> { return fetch_sub(1); }
};

} // namespace detail

// Clang's stdlib provices integral specializations for std::atomic<int128_t>.
// Clang also sets is_always_lock_free to true for int128_t.
template<typename T>
using Atomic16B = std::conditional_t<std::atomic<T>::is_always_lock_free,
                                     std::atomic<T>, detail::GccAtomic16B<T>>;

} // namespace hogd
