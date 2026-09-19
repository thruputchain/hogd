
#include <gtest/gtest.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <random>
#include <ranges>
#include <utility>

#include <x23/cleanup.h>
#include <x23/debug.h>
#include <x23/err.h>
#include <x23/fileio.h>
#include <x23/uiobuf.h>
#include <x23/utility.h>
#include <x23/vec.h>

namespace x23 {
struct AltBufVec {
  static constexpr size_t kCopyThreshold = 4096;

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
  void swap(AltBufVec &other) noexcept
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
      back.reserve(std::max(chunk_size(), b.size()));
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
        n = std::max(n, chunk_size());
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

  std::expected<IoResult, Errno> write(BorrowedFd fd)
  {
    using enum IoResult;
    while (!empty()) {
      auto iov{to_vec()};
      auto n =
          writev(fd.get(), iov.data(), std::min<size_t>(iov.size(), IOV_MAX));
      if (n < 0) {
        if (errno == EAGAIN)
          return kAgain;
        Errno ret;
        clear();
        return ret;
      }
      trim(n);
    }
    return kComplete;
  }

  // Returns false on full write, true on EAGAIN, error otherwise.
  template<typename Buf>
  std::expected<IoResult, Errno> write_or_append(BorrowedFd fd, Buf &&buf)
      requires requires { append(std::forward<Buf>(buf)); }
  {
    using enum IoResult;
    if (!empty() || buf.empty()) {
      append(std::forward<Buf>(buf));
      return kQueued;
    }

    for (size_t pos = 0, end = buf.size(); pos != end;) {
      auto n = ::write(fd.get(), buf.data() + pos, end - pos);
      if (n < 0) {
        if (Errno err; err != EAGAIN) {
          clear();
          return err;
        }
        append(std::forward<Buf>(buf), pos);
        return kAgain;
      }
      pos += n;
    }
    return kComplete;
  }

private:
  bool backfull(size_t n) const
  {
    if (empty())
      return true;
    const auto &back = bufs_.back();
    return back.capacity() - back.size() < n;
  }

  size_t chunk_size() const
  {
    static constexpr size_t kMinChunk = 2 * kCopyThreshold;
    if (bufs_.empty())
      return kMinChunk;
    return std::max(kMinChunk,
                    std::min<size_t>(2 * bufs_.back().size(), PTRDIFF_MAX));
  }
};
} // namespace x23

namespace {

std::default_random_engine rnd;
std::uniform_int_distribution<std::size_t>
    sizegen(0, 2 * x23::BufVec::kCopyThreshold);

x23::Buf
random_buf()
{
  std::uniform_int_distribution<std::uint8_t> bytegen;
  x23::Buf ret;
  ret.resize_uninitialized(sizegen(rnd));
  std::ranges::generate(ret, [&] { return bytegen(rnd); });
  return ret;
}

bool
buf_eq(x23::BufView flat, const auto &iov)
{
  for (const auto &v : iov) {
    if (flat.size() < v.iov_len)
      return false;
    if (v.iov_len > 0 && memcmp(flat.data(), v.iov_base, v.iov_len) != 0)
      return false;
    flat = flat.subspan(v.iov_len);
  }
  return flat.empty();
}

} // namespace

TEST(uiobuf, SimpleAppend)
{
  x23::Buf flat;
  x23::BufVec v;
  for (int i = 0; i < 100; ++i) {
    auto b = random_buf();
    flat += b;
    v.append(std::move(b));
    ASSERT_TRUE(buf_eq(flat, v.to_vec()));
  }

  x23::BufView flatview(flat);
  EXPECT_EQ(flat.data(), flatview.data());
  EXPECT_EQ(flat.size(), flatview.size());
  ASSERT_TRUE(buf_eq(flatview, v.to_vec()));

  while (!flatview.empty()) {
    auto sz = std::min(sizegen(rnd), flatview.size());
    flatview = flatview.subspan(sz);
    v.trim(sz);
    EXPECT_TRUE(buf_eq(flatview, v.to_vec()));
  }
}

TEST(uiobuf, CoalesceAdvancesStorageIovnum)
{
  x23::Buf first{std::uint8_t{0x11}};
  x23::Buf second{std::uint8_t{0x22}};
  x23::BufVec v;
  v.append(first);

  // Reproduce the bookkeeping state in which a reused Storage coalesces
  // with a newer iovec.  Construct it explicitly so the test does not
  // depend on the allocator placing two allocations next to each other.
  auto &back = v.storage_.back();
  v.data_.emplace_back(iovec{
      .iov_base = back.bytes_.data() + back.bytes_.size(),
      .iov_len = 0,
  });
  v.append(second);

  ASSERT_EQ(v.to_vec().size(), std::size_t{2});
  v.trim(first.size());

  // Consuming the old iovec must not release Storage backing the newer one.
  ASSERT_EQ(v.storage_.size(), std::size_t{1});
  EXPECT_TRUE(buf_eq(second, v.to_vec()));
}

TEST(uiobuf, Swap)
{
  x23::Buf flat;
  x23::BufVec v, v2;
  for (int i = 0; i < 100; ++i) {
    auto b = random_buf();
    flat += b;
    v2.append(b);
    v.append(std::move(b));
    ASSERT_TRUE(buf_eq(flat, v.to_vec()));
  }

  x23::BufView flatview(flat);

  while (!flatview.empty()) {
    auto sz = std::min(sizegen(rnd), flatview.size());

    x23::BufVec v3;
    v3.swap(v);
    v3.trim(sz);
    v2.swap(v3);
    v3.swap(v);
    v.swap(v2);

    EXPECT_TRUE(buf_eq(flatview, v2.to_vec()));
    v2.trim(sz);
    flatview = flatview.subspan(sz);
    EXPECT_TRUE(buf_eq(flatview, v2.to_vec()));
    EXPECT_TRUE(buf_eq(flatview, v.to_vec()));
  }
}

TEST(VecQ, CompactsConsumedPrefixBeforeGrowing)
{
  x23::VecQ<int> q;
  q.reserve(32);
  auto *original = q.data();
  auto capacity = q.capacity();
  q.resize(capacity);
  for (size_t i = 0; i < capacity; ++i)
    q.data()[i] = int(i);
  q.pop_front(capacity - 2);
  q.reserve(4);
  EXPECT_EQ(q.data(), original);
  ASSERT_EQ(q.size(), 2UZ);
  EXPECT_EQ(q.data()[0], int(capacity - 2));
  EXPECT_EQ(q.data()[1], int(capacity - 1));
  q.emplace_back(99);
  EXPECT_EQ(q.back(), 99);
}

TEST(VecQ, SwapPreservesOffsets)
{
  x23::VecQ<int> a, b;
  for (int n : {1, 2, 3})
    a.emplace_back(n);
  for (int n : {4, 5})
    b.emplace_back(n);
  a.pop_front(2);
  b.pop_front(1);
  auto *ap = a.data();
  auto *bp = b.data();
  a.swap(b);
  EXPECT_EQ(a.data(), bp);
  EXPECT_EQ(b.data(), ap);
  EXPECT_EQ(a.size(), 1UZ);
  EXPECT_EQ(b.size(), 1UZ);
  EXPECT_EQ(*a.begin(), 5);
  EXPECT_EQ(a.front(), 5);
  EXPECT_EQ(a.at(0), 5);
  EXPECT_EQ(std::as_const(a)[0], 5);
  EXPECT_THROW(a.at(1), std::out_of_range);
  EXPECT_EQ(*b.begin(), 3);
}

TEST(uiobuf, EmptyAppendsDoNotQueueWrites)
{
  x23::BufVec buffers;
  buffers.append(x23::BufView{});
  ASSERT_TRUE(buffers.empty());
  buffers.append(x23::Buf{});
  ASSERT_TRUE(buffers.empty());
  x23::Buf data{1, 2};
  buffers.append(data, data.size());
  ASSERT_TRUE(buffers.empty());
  EXPECT_EQ(buffers.write(-1).value(), x23::IoResult::kComplete);
}

TEST(uiobuf, SelfMovePreservesQueuedData)
{
  x23::BufVec buffers;
  x23::Buf data{1, 2, 3};
  buffers.append(data);
  auto &alias = buffers;
  buffers = std::move(alias);
  EXPECT_EQ(buffers.nbytes(), 3UZ);
  auto iov = buffers.to_vec();
  ASSERT_EQ(iov.size(), 1UZ);
  EXPECT_EQ(std::memcmp(iov[0].iov_base, data.data(), data.size()), 0);
}
