// -*- C++ -*-

#pragma once

#include <sys/uio.h>

#include <x23/vec.h>

namespace x23 {

// VecQ is a vector that lets you pop from the front as well as from
// the back.
template<typename T> class VecQ : protected Vec<T> {
  using super = Vec<T>;
public:
  using typename super::const_iterator;
  using typename super::const_pointer;
  using typename super::const_reference;
  using typename super::difference_type;
  using typename super::iterator;
  using typename super::pointer;
  using typename super::reference;
  using typename super::size_type;
  using typename super::value_type;

  using super::super;
  VecQ(VecQ &&) noexcept = default;

  pointer data() noexcept { return super::data() + offset_; }
  size_t size() const noexcept { return super::size() - offset_; }

  auto begin(this auto &&self) noexcept
  {
    return self.super::begin() + self.offset_;
  }
  using super::end;

  template<typename... Args> reference emplace_back(Args &&...args)
  {
    ensure();
    return super::emplace_back(std::forward<Args>(args)...);
  }
  void pop_front(size_type n = 1) noexcept
  {
    if (auto cmp = n <=> size(); cmp == -1)
      offset_ += n;
    else {
      assert(cmp == 0);
      offset_ = 0;
      super::clear();
    }
  }
  using super::pop_back;

private:
  size_type offset_;

  void ensure()
  {
    if (super::size() == super::capacity()) {
      if (offset_ > super::size() / 2) {
        erase(super::begin(), begin());
        offset_ = 0;
      }
      else
	super::ensure(1);
    }
  }
};

} // namespace x23
