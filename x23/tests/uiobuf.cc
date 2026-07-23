
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

// Regression test for issue #3113.  Copied ranges are only coalesced into
// the previous iovec when they share the same Storage allocation.  Two
// distinct Storage allocations must never be merged into one iovec, even
// if the allocator happens to place them contiguously, because the merged
// iovec would then span two arrays and later pointer arithmetic from its
// base would leave the first array.
TEST(uiobuf, CoalesceStaysWithinStorage)
{
  x23::Buf byte;
  byte.resize(1, 0xcd);

  x23::Buf flat;
  x23::BufVec v;

  // The first Storage reserves 2 * kCopyThreshold bytes; fill it exactly
  // with single-byte copies, all of which live in one allocation and so
  // coalesce into a single iovec.
  constexpr std::size_t kFirst = 2 * x23::BufVec::kCopyThreshold;
  for (std::size_t i = 0; i < kFirst; ++i) {
    v.append(x23::BufView(byte));
    flat += byte;
  }
  {
    auto iov = v.to_vec();
    ASSERT_EQ(iov.size(), std::size_t{1});
    EXPECT_EQ(iov.front().iov_len, kFirst);
  }

  // One more copy spills into a freshly allocated second Storage.  It must
  // start a new iovec rather than extending the first one, regardless of
  // where the allocator placed the second allocation (the bug was to merge
  // when the two allocations happened to be adjacent in memory).
  v.append(x23::BufView(byte));
  flat += byte;

  auto iov = v.to_vec();
  ASSERT_EQ(iov.size(), std::size_t{2});
  EXPECT_EQ(iov.front().iov_len, kFirst);
  EXPECT_EQ(iov.back().iov_len, std::size_t{1});

  // Data must round-trip, and every iovec must lie entirely within one
  // Storage allocation.
  ASSERT_TRUE(buf_eq(flat, iov));
  for (const auto &v0 : iov) {
    const auto *base = static_cast<const std::uint8_t *>(v0.iov_base);
    const bool contained =
        std::ranges::any_of(v.storage_, [&](const x23::BufVec::Storage &s) {
          const auto *lo = s.bytes_.data();
          const auto *hi = lo + s.bytes_.capacity();
          return base >= lo && base + v0.iov_len <= hi;
        });
    EXPECT_TRUE(contained);
  }
}
