// -*- C++ -*-

#pragma once

#include <algorithm>
#include <cerrno>
#include <climits> // for IOV_MAX
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <span>
#include <sys/uio.h>
#include <unistd.h>

#include <utility>
#include <x23/cleanup.h>
#include <x23/debug.h>
#include <x23/err.h>
#include <x23/fileio.h>
#include <x23/utility.h>
#include <x23/vec.h>

namespace x23 {

enum class IoResult {
  kComplete, // Nothing more to do
  kQueued,   // No IO attempted, data added to queue
  kAgain,    // Received EAGAIN error
};

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
      x23assert(n == sz);
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

  struct Storage {
    Buf bytes_;
    size_t iovnum_; // can't destroy nconsumed_ > iovnum_
    size_t room() const noexcept { return bytes_.capacity() - bytes_.size(); }
    uint8_t *copy(const uint8_t *p, size_t sz)
    {
      x23assert(sz <= room());
      size_t offset = bytes_.size();
      bytes_.resize_uninitialized(offset + sz);
      uint8_t *ret = bytes_.data() + offset;
      if (sz > 0) {
        std::memcpy(ret, p, sz);
      }
      return ret;
    }
  };

  VecQ<Storage> storage_;
  VecQ<iovec> data_;
  size_t nconsumed_{}; // number of iovecs consumed
  // Base of the Storage::bytes_ allocation backing the last iovec in
  // data_.  Used to decide whether a freshly copied range may be
  // coalesced with the previous iovec: pointer adjacency alone is not
  // enough, since the allocator may place distinct allocations
  // contiguously.  Coalescing is only sound within a single allocation.
  const uint8_t *last_iov_storage_{};

  BufVec() noexcept = default;
  BufVec(BufVec &&other) noexcept { swap(other); }
  BufVec &operator=(BufVec &&other) noexcept
  {
    if (this != &other) {
      clear();
      swap(other);
    }
    return *this;
  }

  void clear()
  {
    data_.clear();
    storage_.clear();
    nconsumed_ = 0;
    last_iov_storage_ = nullptr;
  }

  bool empty() const noexcept { return data_.empty(); }

  void swap(BufVec &other) noexcept
  {
    using std::swap;
    swap(storage_, other.storage_);
    swap(data_, other.data_);
    swap(nconsumed_, other.nconsumed_);
    swap(last_iov_storage_, other.last_iov_storage_);
  }

  size_t nbytes() const noexcept
  {
    size_t ret = 0;
    for (const auto &v : data_)
      ret += v.iov_len;
    return ret;
  }

  void append(BufView b, size_t offset = 0)
  {
    x23assert(b.size() >= offset);
    const size_t nbytes = b.size() - offset;
    if (storage_.empty())
      storage_.emplace_back().bytes_.reserve(
          std::max(2 * kCopyThreshold, nbytes));
    else if (storage_.back().room() < nbytes)
      storage_.emplace_back().bytes_.reserve(
          std::max(nbytes,
                   std::min<size_t>(PTRDIFF_MAX,
                                    2 * storage_.back().bytes_.capacity())));
    Storage &back = storage_.back();
    auto *p = back.copy(b.data() + offset, nbytes);
    // Only coalesce with the previous iovec when this copy landed in the
    // same Storage allocation.  Pointer adjacency across distinct
    // allocations can hold by chance, but extending the iovec would make
    // it span two arrays, and later pointer arithmetic from its base
    // would leave the first array (undefined behavior).
    if (!data_.empty() && back.bytes_.data() == last_iov_storage_ &&
        p == static_cast<uint8_t *>(data_.back().iov_base) +
                 data_.back().iov_len)
      data_.back().iov_len += nbytes;
    else {
      back.iovnum_ = iovnum();
      data_.emplace_back(iovec{
          .iov_base = p,
          .iov_len = nbytes,
      });
    }
    last_iov_storage_ = back.bytes_.data();
  }

  void append(Buf &&b, size_t offset = 0)
  {
    if (b.size() - offset < kCopyThreshold) {
      append(BufView(b), offset);
      return;
    }
    size_t backroom = 0;
    if (!storage_.empty())
      backroom = storage_.back().room();
    Storage *newback = &storage_.emplace_back(std::move(b), iovnum());
    data_.emplace_back(iovec{
        .iov_base = newback->bytes_.data() + offset,
        .iov_len = newback->bytes_.size() - offset,
    });
    // The new iovec is backed by this owned Buf's allocation; record it so
    // a later small copy into the same Storage may legitimately coalesce.
    last_iov_storage_ = newback->bytes_.data();

    // Keep the Storage with the most room at the back of the list.
    // It's okay to have iovnum_ out of order.
    if (backroom > newback->room())
      std::swap(newback[0], newback[-1]);
  }

  std::span<const iovec> to_vec() const { return data_; }

  void trim(size_t n)
  {
    while (n > 0) {
      auto &v = data_.front();
      if (n < v.iov_len) {
        v.iov_base = static_cast<std::byte *>(v.iov_base) + n;
        v.iov_len -= n;
        break;
      }
      n -= v.iov_len;
      data_.pop_front(1);
      ++nconsumed_;
    }
    if (data_.empty()) {
      clear();
      return;
    }
    size_t nfree = 0, e = storage_.size();
    while (nfree < e && storage_[nfree].iovnum_ < nconsumed_)
      ++nfree;
    storage_.pop_front(nfree);
  }

  std::expected<IoResult, Errno> write(BorrowedFd fd)
  {
    using enum IoResult;
    while (!data_.empty()) {
      auto iov{to_vec()};
      auto n =
          writev(fd.get(), iov.data(), std::min<size_t>(iov.size(), IOV_MAX));
      if (n < 0) {
        if (errno == EAGAIN)
          return kAgain;
        Errno ret;
        debug("writev(fd {}, niov {}): {}", fd.get(), ret, iov.size());
        clear();
        return ret;
      }
      trim(n);
    }
    return kComplete;
  }

private:
  size_t iovnum() const { return nconsumed_ + data_.size(); }
};

} // namespace x23
