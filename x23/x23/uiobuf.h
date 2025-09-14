// -*- C++ -*-

#pragma once

#include <expected>

#include <sys/uio.h>
#include <unistd.h>

#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/fileio.h>
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
  VecQ &operator=(VecQ &&other) noexcept
  {
    super::operator=(std::move(other));
    offset_ = std::exchange(other.offset_, 0);
    return *this;
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
  void resize(size_type new_size)
  {
    if (new_size)
      super::resize(offset_ + new_size);
    else
      clear();
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
  void pop_front(size_type n) noexcept
  {
    if (auto sz = size(); n < sz)
      offset_ += n;
    else {
      assert(n == sz);
      clear();
    }
  }
  value_type pop_return_front() noexcept
  {
    value_type ret = std::move(front);
    pop_front(1);
    return ret;
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
  using super::resize_and_overwrite;

  size_t capacity() const noexcept { return super::capacity() - offset_; }
  void reserve(size_type cap)
  {
    if (cap <= capacity())
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
static_assert(is_bytevec<VecQ<std::uint8_t>>);

// This is a vector of buffers that can be turned into an array of
// iovec structures for use with writev.  Lets you trim an arbitrary
// number of bytes off the front.  Copies Bufs less than
// kCopyThreshold, but will assume ownership of larger Bufs moved in.
struct BufVec {
  static constexpr size_t kCopyThreshold = 4096;
  static constexpr size_t kChunkSize = 2 * kCopyThreshold;

  VecQ<Buf> bufs_;
  size_t skip_{};

  bool empty() const noexcept { return bufs_.empty(); }
  size_t nbytes() const noexcept
  {
    size_t ret = -skip_;
    for (const auto &b : bufs_)
      ret += b.size();
    return ret;
  }
  void clear()
  {
    bufs_.clear();
    skip_ = 0;
  }
  void swap(BufVec &other) noexcept
  {
    bufs_.swap(other.bufs_);
    std::swap(skip_, other.skip_);
  }

  void append(BufView b, size_t offset = 0)
  {
    if (offset)
      b = b.subspan(offset);
    if (backfull(b.size())) {
      Buf &back = bufs_.emplace_back();
      back.reserve(std::max(kChunkSize, b.size()));
      back.append_range(b);
    }
    else
      bufs_.back().append_range(b);
  }
  void append(Buf &&b, size_t offset = 0)
  {
    if (b.size() >= kCopyThreshold && (offset == 0 || empty())) {
      bufs_.emplace_back(std::move(b));
      if (offset)
        trim(offset);
    }
    else
      append(BufView{b}, offset);
  }

  template<std::ranges::forward_range R>
  void move_append_range(R &&r) requires requires {
    { std::move(*r.begin()) } -> std::same_as<Buf &&>;
  }
  {
    auto it = r.begin();
    while (it != r.end() && !backfull(r.size())) {
      bufs_.back().append_range(*it);
      (*it).clear();
      ++it;
    }

    while (it != r.end()) {
      if ((*it).size() >= kCopyThreshold) {
        bufs_.emplace_back(std::move(*it++));
        continue;
      }

      size_t n = 0;
      auto eit = it;
      while (eit != r.end() && (*eit).size() < kCopyThreshold) {
        n += (*eit).size();
        ++eit;
      }
      if (eit == r.end())
        n = std::max(n, kChunkSize);
      Buf &back = bufs_.emplace_back();
      back.reserve(n);
      while (it != eit) {
        back.append_range(*it);
        (*it).clear();
        ++it;
      }
    }
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
      bufs_.pop_front(1);
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

  // Returns true if the buffer is not empty and you need to call
  // write again as soon as fd is writable.
  std::expected<bool, Errno> write(BorrowedFd fd)
  {
    if (empty())
      return false;
    auto iov{to_vec()};
    if (auto n = writev(fd.get(), iov.data(), iov.size()); n > 0) {
      trim(n);
      return !empty();
    }
    if (errno == EAGAIN) // XXX not appropriate for srpc
      return true;
    Errno ret;
    clear();
    return ret;
  }

  // Returns true if the buffer was empty and is no longer, so you
  // need to call write when fd is writable.  If the buffer was
  // already not empty, it is assumed that there was already a call to
  // write scheduled, so does not even attempt a write and returns
  // false.
  template<typename Buf>
  std::expected<bool, Errno> write_or_append(BorrowedFd fd, Buf &&buf)
      requires requires { append(std::forward<Buf>(buf)); }
  {
    if (empty()) {
      auto n = ::write(fd.get(), buf.data(), buf.size());
      if (n < 0) {
        if (errno != EAGAIN) {
          Errno ret;
          clear();
          return ret;
        }
        n = 0;
      }
      if (size_t(n) < buf.size()) {
        append(std::forward<Buf>(buf), n);
        return true;
      }
    }
    else
      append(std::forward<Buf>(buf));
    return false;
  }

private:
  bool backfull(size_t n) const
  {
    if (empty())
      return true;
    const auto &back = bufs_.back();
    return back.capacity() - back.size() < n;
  }
};

} // namespace x23
