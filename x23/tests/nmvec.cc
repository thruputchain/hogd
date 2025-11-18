#include <cstddef>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "x23/nmvec.h"

using x23::NMVec;

template<int N = 0> struct CountingT {
  int value{};

  inline static int ctor_count = 0;
  inline static int dtor_count = 0;
  inline static int copy_count = 0;
  inline static int move_count = 0;

  CountingT() { ++ctor_count; }

  explicit CountingT(int v) : value(v) { ++ctor_count; }

  CountingT(const CountingT &other) : value(other.value)
  {
    ++ctor_count;
    ++copy_count;
  }

  CountingT(CountingT &&other) noexcept : value(other.value)
  {
    ++ctor_count;
    ++move_count;
  }

  CountingT &operator=(const CountingT &)
  {
    ++copy_count;
    return *this;
  }
  CountingT &operator=(CountingT &&) noexcept
  {
    ++move_count;
    return *this;
  }

  ~CountingT() { ++dtor_count; }

  static void reset() { ctor_count = dtor_count = copy_count = move_count = 0; }

  static int live() { return ctor_count - dtor_count; }
};

struct ThrowingT {
  int value{};

  inline static int ctor_count = 0;
  inline static int dtor_count = 0;
  inline static int throw_after = -1; // ctor index after which to throw

  ThrowingT() { maybe_construct(); }

  explicit ThrowingT(int v) : value(v) { maybe_construct(); }

  ThrowingT(const ThrowingT &other) : value(other.value) { maybe_construct(); }

  ThrowingT(ThrowingT &&other) noexcept : value(other.value)
  {
    maybe_construct();
  }

  ThrowingT &operator=(const ThrowingT &) = default;
  ThrowingT &operator=(ThrowingT &&) noexcept = default;

  ~ThrowingT() { ++dtor_count; }

  static void reset()
  {
    ctor_count = 0;
    dtor_count = 0;
    throw_after = -1;
  }

  static int live() { return ctor_count - dtor_count; }

  static void set_throw_after(int n) { throw_after = n; }

private:
  void maybe_construct()
  {
    if (throw_after >= 0 && ctor_count >= throw_after)
      throw std::runtime_error("ThrowingT ctor");
    ++ctor_count;
  }
};

inline bool
operator==(const ThrowingT &a, const ThrowingT &b)
{
  return a.value == b.value;
}

template<int N1, int N2>
inline bool
operator==(const CountingT<N1> &a, const CountingT<N2> &b)
{
  return a.value == b.value;
}

template<typename Vec1, typename Vec2>
bool
equal(const Vec1 &a, const Vec2 &b)
{
  size_t sz = a.size();
  if (sz != b.size())
    return false;
  for (std::size_t i = 0; i < sz; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

TEST(NMVec, BasicSequences)
{
  CountingT<0>::reset();
  CountingT<1>::reset();

  NMVec<CountingT<0>> nm;
  std::vector<CountingT<1>> ref;

  // 1) push 100 elements
  for (int i = 0; i < 100; ++i) {
    auto idx1 = nm.emplace_back(i);
    ref.emplace_back(i);
    EXPECT_EQ(idx1, i);
    EXPECT_EQ(nm.size(), ref.size());
    EXPECT_TRUE(equal(nm, ref));
    EXPECT_EQ(nm.size(), CountingT<0>::live());
    EXPECT_EQ(ref.size(), CountingT<1>::live());
    EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());
  }

  // 2) grow via resize
  nm.resize(150);
  ref.resize(150);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(nm.size(), CountingT<0>::live());
  EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());

  // 3) shrink via resize
  nm.resize(37);
  ref.resize(37);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(nm.size(), CountingT<0>::live());
  EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());

  // 4) mutate some elements in place
  for (std::size_t i = 0; i < nm.size(); ++i) {
    nm[i].value += 7;
    ref[i].value += 7;
  }
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());

  // 5) grow and shrink again
  nm.resize(200);
  ref.resize(200);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(nm.size(), CountingT<0>::live());
  EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());

  nm.resize(0);
  ref.resize(0);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(nm.size(), CountingT<0>::live());
  EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());
}

TEST(NMVec, LifetimeInvariant)
{
  CountingT<>::reset();

  NMVec<CountingT<>> nm;

  // Build some elements
  for (int i = 0; i < 100; ++i)
    nm.emplace_back(i);

  EXPECT_EQ(CountingT<>::live(), static_cast<int>(nm.size()));

  // Grow
  nm.resize(150);
  EXPECT_EQ(CountingT<>::live(), static_cast<int>(nm.size()));

  // Shrink
  nm.resize(37);
  EXPECT_EQ(CountingT<>::live(), static_cast<int>(nm.size()));

  // Grow and shrink again
  nm.resize(200);
  EXPECT_EQ(CountingT<>::live(), static_cast<int>(nm.size()));

  nm.resize(0);
  EXPECT_EQ(CountingT<>::live(), static_cast<int>(nm.size()));
}

TEST(NMVec, PointerStability)
{
  CountingT<0>::reset();

  NMVec<CountingT<0>> nm;

  // Build some initial elements
  for (int i = 0; i < 100; ++i)
    nm.emplace_back(i);

  EXPECT_EQ(CountingT<0>::live(), static_cast<int>(nm.size()));

  // Capture some pointers into the middle
  CountingT<0> *p10 = &nm[10];
  CountingT<0> *p20 = &nm[20];
  CountingT<0> *p50 = &nm[50];

  int v10 = nm[10].value;
  int v20 = nm[20].value;
  int v50 = nm[50].value;

  // Grow and shrink, but keep size() > 50 so all these indices remain valid
  nm.resize(200);
  nm.resize(150);
  nm.resize(300);
  nm.resize(51);

  EXPECT_EQ(&nm[10], p10);
  EXPECT_EQ(&nm[20], p20);
  EXPECT_EQ(&nm[50], p50);

  EXPECT_EQ(nm[10].value, v10);
  EXPECT_EQ(nm[20].value, v20);
  EXPECT_EQ(nm[50].value, v50);

  EXPECT_EQ(CountingT<0>::live(), static_cast<int>(nm.size()));

  // Now shrink below 20; p20/p50 become dangling by contract.
  nm.resize(15);
  EXPECT_EQ(nm.size(), 15u);
  EXPECT_EQ(CountingT<0>::live(), static_cast<int>(nm.size()));

  nm.resize(0);
  EXPECT_EQ(nm.size(), 0u);
  EXPECT_EQ(CountingT<0>::live(), 0);
}

TEST(NMVec, RandomizedOps)
{
  CountingT<0>::reset();
  CountingT<1>::reset();

  NMVec<CountingT<0>> nm;
  std::vector<CountingT<1>> ref;

  std::mt19937_64 rng(123456);
  std::uniform_int_distribution<int> op_dist(0,
                                             2); // 0: push, 1: grow, 2: shrink
  std::uniform_int_distribution<int> delta_dist(0, 50); // size deltas

  for (int step = 0; step < 2000; ++step) {
    int op = op_dist(rng);

    switch (op) {
    case 0:
    { // emplace_back
      int v = static_cast<int>(nm.size() ^ static_cast<std::size_t>(step));
      auto idx = nm.emplace_back(v);
      ref.emplace_back(v);
      EXPECT_EQ(idx, nm.size() - 1);
      break;
    }

    case 1:
    { // grow via resize
      std::size_t delta = static_cast<std::size_t>(delta_dist(rng));
      std::size_t new_size = nm.size() + delta;
      nm.resize(new_size);
      ref.resize(new_size);
      break;
    }

    case 2:
    { // shrink via resize
      if (nm.size() == 0)
        break;
      std::size_t delta = static_cast<std::size_t>(delta_dist(rng));
      std::size_t new_size = (delta >= nm.size()) ? 0 : nm.size() - delta;
      nm.resize(new_size);
      ref.resize(new_size);
      break;
    }
    }

    EXPECT_TRUE(equal(nm, ref));
    EXPECT_EQ(CountingT<0>::live(), static_cast<int>(nm.size()));
    EXPECT_EQ(CountingT<1>::live(), static_cast<int>(ref.size()));
  }

  nm.resize(0);
  ref.resize(0);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(CountingT<0>::live(), 0);
  EXPECT_EQ(CountingT<1>::live(), 0);
}

TEST(NMVec, ExceptionSafetyEmplaceBack)
{
  ThrowingT::reset();

  NMVec<ThrowingT> nm;

  // First, create some elements without throwing
  ThrowingT::set_throw_after(-1);
  const int initial = 10;
  for (int i = 0; i < initial; ++i)
    nm.emplace_back(i);

  EXPECT_EQ(nm.size(), static_cast<std::size_t>(initial));
  EXPECT_EQ(ThrowingT::live(), static_cast<int>(nm.size()));

  // Now make the constructor start throwing after a few more constructions
  int current_ctors = ThrowingT::ctor_count;
  ThrowingT::set_throw_after(current_ctors + 5);

  std::size_t before_size = nm.size();
  bool threw = false;

  try {
    for (int i = 0; i < 20; ++i)
      nm.emplace_back(1000 + i);
  } catch (const std::runtime_error &) {
    threw = true;
  }

  EXPECT_TRUE(threw);
  // Size should not exceed the count of successfully constructed elements.
  EXPECT_LE(nm.size(), before_size + 5);
  // Live objects should match nm.size().
  EXPECT_EQ(ThrowingT::live(), static_cast<int>(nm.size()));

  // Turn off throwing and ensure we can still grow
  ThrowingT::set_throw_after(-1);
  std::size_t target = nm.size() + 10;
  while (nm.size() < target)
    nm.emplace_back(2000);
  EXPECT_EQ(ThrowingT::live(), static_cast<int>(nm.size()));

  nm.resize(0);
  EXPECT_EQ(nm.size(), 0u);
  EXPECT_EQ(ThrowingT::live(), 0);
}

TEST(NMVec, ExceptionSafetyResizeGrow)
{
  ThrowingT::reset();

  NMVec<ThrowingT> nm;

  // Build some initial elements without throwing
  ThrowingT::set_throw_after(-1);
  const int initial = 20;
  for (int i = 0; i < initial; ++i)
    nm.emplace_back(i);

  EXPECT_EQ(nm.size(), static_cast<std::size_t>(initial));
  EXPECT_EQ(ThrowingT::live(), static_cast<int>(nm.size()));

  // Arrange for throws during resize's default-construction of new elements.
  int current_ctors = ThrowingT::ctor_count;
  ThrowingT::set_throw_after(current_ctors +
                             7); // fail after a few new elements

  std::size_t before_size = nm.size();
  bool threw = false;

  try {
    nm.resize(before_size + 50); // attempt to grow
  } catch (const std::runtime_error &) {
    threw = true;
  }

  EXPECT_TRUE(threw);

  // After exception, size should remain at the old value.
  EXPECT_EQ(nm.size(), before_size);
  EXPECT_EQ(ThrowingT::live(), static_cast<int>(nm.size()));

  // Turn off throwing and grow successfully.
  ThrowingT::set_throw_after(-1);
  nm.resize(before_size + 30);
  EXPECT_EQ(nm.size(), before_size + 30);
  EXPECT_EQ(ThrowingT::live(), static_cast<int>(nm.size()));

  nm.resize(0);
  EXPECT_EQ(nm.size(), 0u);
  EXPECT_EQ(ThrowingT::live(), 0);
}
