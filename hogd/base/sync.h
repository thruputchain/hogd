// sync.h - synchronization utilities

#pragma once

#include <atomic>
#include <concepts>

namespace hogd {

// LossyCounter is concurrent counter that trades the possibility of lost
// increments for speed. Thus, it maintains counts that are <= the actual count.
//
// Generally it is best to use inc() to add one count at a time, since larger
// values could result a greater amount of lost increments.
//
// Using the return value of inc() is also a little faster than an additional
// call to get() but not required.
template<std::integral T> class LossyCounter {
public:
  LossyCounter() : val_(0) {}
  LossyCounter(T val) : val_(val) {}

  // get returns the current count
  T get() const { return val_.load(std::memory_order_relaxed); }

  // inc increments the count by val and returns the current count.
  T inc(T val)
  {
    T tmp = get() + val;
    val_.store(tmp, std::memory_order_relaxed);
    return tmp;
  }

  // inc increments the count by one and returns the current count.
  T inc() { return inc(1); }

  constexpr LossyCounter &operator+=(const T &rhs)
  {
    inc(rhs);
    return *this;
  }
  constexpr bool operator==(const T &rhs) const { return get() == rhs; }
  constexpr auto operator<=>(const T &rhs) const { return get() <=> rhs; }

private:
  std::atomic<T> val_;
};

} // namespace hogd
