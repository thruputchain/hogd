// -*-C++-*-

// A GrowVec is a vector that will grow as needed, without incurring a
// lot of synchronization overhead in the common case that it doesn't
// need to grow.  For instance, it is suitable to represent a vector
// of all file descriptor numbers, since the maximum file descriptor
// number could theoretically grow dynamically, even though this will
// happen versy seldom.

#include <x23/corosync.h>

namespace x23 {

// A pointer that is good only until you suspend the current
// coroutine, which is the duration of the mutual exclusion provided
// by CoroSharedLock::lock_shared.
template<typename T> class CoroTempPtr {
public:
  using value_type = T;

  CoroTempPtr(T *p = nullptr) : p_(p) { init(); }
  CoroTempPtr &operator=(T *p)
  {
    p_ = p;
    init();
    return *this;
  }

  T *get() const
  {
    check();
    return p_;
  }
  operator T &() const { return *get(); }
  T *operator->() const { return get(); }

private:
  T *p_;
#ifndef NDEBUG
  ThreadPool::SuspendCount sc_;
  void init() { sc_ = ThreadPool::get_suspend_count(); }
  void check() const { assert(!p_ || sc_ == ThreadPool::get_suspend_count()); }
#else  // NDEBUG
  void init() {}
  void check() const {}
#endif // NDEBUG
};

// A vector that grows dynamically, but where you can access any
// individual member without synchronization provided you are not
// suspended.
template<typename V> struct GrowVec {
  using value_type = typename V::value_type;
  using pointer = CoroTempPtr<value_type>;

  V v_;
  CoroSharedLock slk_;

  explicit GrowVec(ThreadPool &pool) : slk_(pool) {}

  Async<pointer> operator[](size_t i) {
    for (;;) {
      co_await slk_.lock_shared();
      if (i < v_.size())
	co_return &v_[i];

      CoroGuard g;
      co_await g(slk_);
      if (i <= v_.size())
	v_.resize(i+1);
    }
  }
};

template<typename T>
inline ptrdiff_t
operator-(typename GrowVec<T>::pointer p, const GrowVec<T> &v)
{
  return p.get() - v.v_.data();
}

} // namespace x23
