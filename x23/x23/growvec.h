// -*-C++-*-

// A GrowVec is a vector that will grow as needed, without incurring a
// lot of synchronization overhead in the common case that it doesn't
// need to grow.  For instance, it is suitable to represent a vector
// of all file descriptor numbers, since the maximum file descriptor
// number could theoretically grow dynamically, even though this will
// happen versy seldom.

#pragma once

#include <x23/corosync.h>
#include <x23/testlock.h>

namespace x23 {

// A vector that grows dynamically, but where you can access any
// individual member without synchronization provided you are not
// suspended.
template<typename V> struct GrowVec {
  using value_type = typename V::value_type;
  using pointer = CoroTempPtr<value_type>;

  V v_;
  CoroSharedLock<> slk_;

  explicit GrowVec(ThreadPool &pool) : slk_(pool) {}

  Async<pointer> operator[](size_t i)
  {
    for (;;) {
      auto holder = co_await slk_.coro_lock_shared();
      if (i < v_.size())
        co_return {&v_[i], holder};
      holder.reset();
      CoroGuard guard;
      co_await guard(slk_);
      if (i >= v_.size())
        v_.resize(i + 1);
    }
  }

  Async<pointer> try_at(size_t i)
  {
    auto holder = co_await slk_.coro_lock_shared();
    if (i < v_.size())
      co_return {&v_[i], holder};
    else
      co_return {nullptr, {}};
  }
};

template<typename T>
inline ptrdiff_t
operator-(typename GrowVec<T>::pointer p, const GrowVec<T> &v)
{
  return p.get() - v.v_.data();
}

} // namespace x23
