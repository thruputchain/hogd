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
  VecQ(VecQ &&other) noexcept : super(std::move(other)), offset_(other.offset_)
  {
    other.offset_ = 0;
  }

  pointer data() noexcept { return super::data() + offset_; }
  size_t size() const noexcept { return super::size() - offset_; }
  bool empty(this const auto &self) noexcept { return !self.size(); }
  using super::at;
  using super::operator[];
  using super::front;
  using super::back;
  void clear() noexcept
  {
    super::clear();
    offset_ = 0;
  }

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
  using super::push_back;
  void pop_front(size_type n = 1) noexcept
  {
    if (auto cmp = n <=> size(); cmp == -1)
      offset_ += n;
    else {
      assert(cmp == 0);
      clear();
    }
  }
  using super::pop_back;
  void swap(VecQ &other) noexcept
  {
    super::swap(other);
    std::swap(offset_, other, offset_);
  }
  void resize_uninitialized(size_type new_size)
  {
    reserve(new_size);
    super::resize_uninitialized(new_size + offset_);
  }

  void reserve(size_type cap)
  {
    if (cap < super::capacity() - offset_)
      return;

    size_type totcap = super::capacity();
    if (cap <= totcap / 2) {
      erase(super::begin(), begin());
      offset_ = 0;
      return;
    }

    VecQ oldvec;
    super::swap(*this, oldvec);
    super::reserve(std::max(cap, totcap));
    super::append(std::move_iterator(oldvec.begin()),
                  std::move_iterator(oldvec.end()));
  }

private:
  size_type offset_{};

  void ensure() { reserve(size() + 1); }
};

} // namespace x23
