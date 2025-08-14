#include <gtest/gtest.h>

#include <print>
#include <random>

#include <x23/uiobuf.h>

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
buf_eq(x23::BufView flat, const x23::Vec<iovec> &iov)
{
  for (const auto &v : iov) {
    if (flat.size() < v.iov_len)
      return false;
    if (memcmp(flat.data(), v.iov_base, v.iov_len) != 0)
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
