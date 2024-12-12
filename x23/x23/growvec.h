// -*-C++-*-

// A GrowVec is a vector that will grow as needed, without incuring a
// lot of synchronization overhead in the common case that it doesn't
// need to grow.  For instance, it is suitable to represent a vector
// of all file descriptor numbers, since the maximum file descriptor
// number could theoretically grow dynamically, even though this will
// happen versy seldom.

#include <x23/corosync.h>

namespace x23 {

// A pointer that is good only until you suspend the current
// coroutine, which is the duration of a shared CoroSharedLock..
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
template<typename V> class GrowVec {
public:
  using value_type = typename V::value_type;

  Async<CoroTempPtr<value_type>> operator[](size_t i) {
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
private:
  V v_;
  CoroSharedLock slk_;
};

} // namespace x23
