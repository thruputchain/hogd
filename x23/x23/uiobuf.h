// -*- C++ -*-

#pragma once

#include <expected>

#include <sys/uio.h>
#include <unistd.h>

#include <x23/cleanup.h>
#include <x23/err.h>
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
  const_pointer data() const noexcept { return super::data() + offset_; }
  size_t size() const noexcept { return super::size() - offset_; }
  bool empty(this const auto &self) noexcept { return !self.size(); }
  using super::at;
  using super::operator[];
  using super::back;
  using super::front;
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
    reserve(size() + 1);
    return super::emplace_back(std::forward<Args>(args)...);
  }
  using super::push_back;
  void pop_front(size_type n = 1) noexcept
  {
    if (auto sz = size(); n < sz)
      offset_ += n;
    else {
      assert(n == sz);
      clear();
    }
  }
  using super::pop_back;
  void swap(VecQ &other) noexcept
  {
    super::swap(other);
    std::swap(offset_, other.offset_);
  }
  void resize_uninitialized(size_type new_size)
  {
    reserve(new_size);
    super::resize_uninitialized(new_size + offset_);
  }

  void reserve(size_type cap)
  {
    if (cap <= super::capacity() - offset_)
      return;

    size_type totcap = super::capacity();
    if (cap <= totcap / 2) {
      super::erase(super::begin(), begin());
      offset_ = 0;
      return;
    }

    VecQ oldvec;
    super::swap(oldvec);
    super::reserve(std::max(cap, totcap));
    super::append(std::move_iterator(oldvec.begin()),
                  std::move_iterator(oldvec.end()));
  }

private:
  size_type offset_{};
};

struct BufVec {
  static constexpr size_t kCopyThreshold = 4096;

  VecQ<Buf> bufs_{Buf{}};
  size_t skip_{};

  void append(BufView b)
  {
    if (backfull(b.size()))
      bufs_.emplace_back(std::from_range, b);
    else
      bufs_.back().append_range(b);
  }
  void append(Buf &&b)
  {
    if (b.size() >= kCopyThreshold || backfull(b.size()))
      bufs_.emplace_back(std::move(b));
    else
      append(BufView{b});
  }

  void trim(size_t n)
  {
    while (!bufs_.empty()) {
      size_t frontsize = bufs_.front().size();
      if (n < frontsize - skip_) {
        skip_ += n;
        return;
      }
      n -= frontsize - skip_;
      skip_ = 0;
      bufs_.pop_front();
    }
  }

  Vec<iovec> to_vec()
  {
    Vec<iovec> ret;
    if (!bufs_.empty()) {
      ret.reserve(bufs_.size());
      ret.emplace_back();
      ret.back().iov_base = bufs_.front().data() + skip_;
      ret.back().iov_len = bufs_.front().size() - skip_;
      for (size_t i = 1, e = bufs_.size(); i < e; ++i) {
        ret.emplace_back();
        ret.back().iov_base = bufs_[i].data();
        ret.back().iov_len = bufs_[i].size();
      }
    }
    return ret;
  }

  std::expected<void, Errno> write(BorrowedFd fd)
  {
    auto iov{to_vec()};
    if (auto n = writev(fd.get(), iov.data(), iov.size()); n > 0) {
      trim(n);
      return {};
    }
    return Errno{};
  }

private:
  bool backfull(size_t n) const
  {
    if (bufs_.empty())
      return true;
    const auto &back = bufs_.back();
    auto backsize = back.size();
    return backsize >= kCopyThreshold && back.capacity() - backsize < n;
  }
};

} // namespace x23
