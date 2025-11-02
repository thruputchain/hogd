#pragma once

#include <atomic>
#include <type_traits>

#include "hogd/base/types.h"

namespace hogd {

// gcc after version 7.1 refuses to emit cmpxchg16b instructions,
// instead defaulting to a lock table.
// See gcc bug https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80878

namespace detail {

// Due to the limitations of gcc, this winds up being a full seq_cst fence
// on each op.
template<typename T> requires (sizeof(T) == 16 && alignof(T) == 16 &&
                               std::is_trivially_copyable_v<T>)
class GccAtomic16B {
public:
  GccAtomic16B(T value) : value_(value) {}

  GccAtomic16B(GccAtomic16B const &) = delete;
  GccAtomic16B(GccAtomic16B &&) = delete;
  GccAtomic16B &operator=(GccAtomic16B const &) = delete;
  GccAtomic16B &operator=(GccAtomic16B &&) = delete;

  T
  load(std::memory_order /*unused*/ = std::memory_order_seq_cst) const noexcept
  {
    return __sync_compare_and_swap(&value_, 0, 0);
  }

  void store(T value,
             std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    exchange(value, order);
  }

  T exchange(T value,
             std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    // Per the GCC documentation, this function is an acquire barrier, not a
    // seq_cst barrier.
    T old_value = __sync_lock_test_and_set(&value_, value);

    if (order == std::memory_order_seq_cst) {
      __sync_synchronize();
    }

    return old_value;
  }

  bool compare_exchange_strong(
      T &expected, T desired,
      std::memory_order /*unused*/ = std::memory_order_seq_cst) noexcept
  {

    T previous_value = __sync_val_compare_and_swap(&value_, expected, desired);
    bool success = previous_value == expected;
    expected = previous_value;
    return success;
  }

  bool compare_exchange_weak(
      T &expected, T desired,
      std::memory_order /*unused*/ = std::memory_order_seq_cst) noexcept
  {
    return compare_exchange_strong(expected, desired);
  }

  // specializations for integral types
  T fetch_add(T value,
              std::memory_order /*unused*/ = std::memory_order_seq_cst) noexcept
      requires std::is_integral_v<T>
  {
    return __sync_fetch_and_add(&value_, value);
  }

  T fetch_sub(T value,
              std::memory_order /*unused*/ = std::memory_order_seq_cst) noexcept
      requires std::is_integral_v<T>
  {
    return __sync_fetch_and_sub(&value_, value);
  }

  T fetch_and(T value,
              std::memory_order /*unused*/ = std::memory_order_seq_cst) noexcept
      requires std::is_integral_v<T>
  {
    return __sync_fetch_and_and(&value_, value);
  }

  T fetch_or(T value,
             std::memory_order /*unused*/ = std::memory_order_seq_cst) noexcept
      requires std::is_integral_v<T>
  {
    return __sync_fetch_and_or(&value_, value);
  }

  T fetch_xor(T value,
              std::memory_order /*unused*/ = std::memory_order_seq_cst) noexcept
      requires std::is_integral_v<T>
  {
    return __sync_fetch_and_xor(&value_, value);
  }

private:
  T value_;
};

static_assert(sizeof(GccAtomic16B<int128_t>) == 16);

} // namespace detail

template<typename T>
using Atomic16B = std::conditional_t<std::atomic<T>::is_always_lock_free,
                                     std::atomic<T>, detail::GccAtomic16B<T>>;

} // namespace hogd
