#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <random>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <gtest/gtest.h>

#include "x23/nmvec.h"

using x23::NMVec;

static_assert(std::random_access_iterator<NMVec<int>::iterator>);
static_assert(std::ranges::random_access_range<NMVec<int>>);
static_assert(std::ranges::random_access_range<const NMVec<int>>);
static_assert(std::same_as<std::ranges::range_reference_t<const NMVec<int>>,
                           const int &>);

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
    EXPECT_EQ(idx1, size_t(i));
    EXPECT_EQ(nm.size(), ref.size());
    EXPECT_TRUE(equal(nm, ref));
    EXPECT_EQ(nm.size(), size_t(CountingT<0>::live()));
    EXPECT_EQ(ref.size(), size_t(CountingT<1>::live()));
    EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());
  }

  // 2) grow via resize
  nm.resize(150);
  ref.resize(150);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(nm.size(), size_t(CountingT<0>::live()));
  EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());

  // 3) shrink via resize
  nm.resize(37);
  ref.resize(37);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(nm.size(), size_t(CountingT<0>::live()));
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
  EXPECT_EQ(nm.size(), size_t(CountingT<0>::live()));
  EXPECT_EQ(CountingT<0>::live(), CountingT<1>::live());

  nm.resize(0);
  ref.resize(0);
  EXPECT_TRUE(equal(nm, ref));
  EXPECT_EQ(nm.size(), size_t(CountingT<0>::live()));
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

TEST(NMVec, RangeBasedIteration)
{
  NMVec<int> nm;
  constexpr int kCount = 64;

  for (int i = 0; i < kCount; ++i)
    nm.emplace_back(i);

  // Range-for should hand out mutable references.
  for (auto &value : nm)
    value += 10;

  int expected = 10;
  for (auto value : nm)
    EXPECT_EQ(value, expected++);

  auto first = nm.begin();
  auto last = nm.end();
  EXPECT_EQ(last - first, static_cast<std::ptrdiff_t>(nm.size()));
  EXPECT_EQ(first + static_cast<std::ptrdiff_t>(nm.size()), last);
  EXPECT_EQ(std::ranges::distance(nm), static_cast<std::ptrdiff_t>(nm.size()));

  // Random-access operations should behave the same as for vectors.
  EXPECT_EQ(*(nm.begin() + 5), 15);
  EXPECT_TRUE(std::ranges::is_sorted(nm));
}

TEST(NMVec, ConstIteratorBasics)
{
  NMVec<int> nm;
  constexpr int kCount = 32;
  for (int i = 0; i < kCount; ++i)
    nm.emplace_back(i);

  const NMVec<int> &cnm = nm;
  static_assert(
      std::is_same_v<decltype(cnm.begin()), NMVec<int>::const_iterator>);
  static_assert(
      std::is_const_v<std::remove_reference_t<decltype(*cnm.begin())>>);

  NMVec<int>::const_iterator cit = nm.begin(); // implicit conversion
  EXPECT_EQ(*cit, 0);

  int expected = 0;
  for (int value : cnm)
    EXPECT_EQ(value, expected++);

  auto first = cnm.begin();
  auto last = cnm.end();
  EXPECT_EQ(last - first, static_cast<std::ptrdiff_t>(cnm.size()));
  first += 7;
  EXPECT_EQ(*first, 7);
  EXPECT_EQ(last - first, static_cast<std::ptrdiff_t>(cnm.size() - 7));
}

TEST(NMVec, IterComparisons)
{
  NMVec<int> v1, v2;
  EXPECT_EQ(v1.begin(), v1.end());
  EXPECT_NE(v1.begin(), v2.begin());
  v1.emplace_back(0);
  EXPECT_NE(v1.begin(), v1.end());
  EXPECT_LT(v1.begin(), v1.end());
#ifndef NDEBUG
  struct rlimit core{};
  getrlimit(RLIMIT_CORE, &core);
  if (core.rlim_cur) {
    auto nocore = core;
    nocore.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &nocore);
  }
  EXPECT_DEATH(void(v1.begin() <= v2.begin()), "");
  if (core.rlim_cur)
    setrlimit(RLIMIT_CORE, &core);
#endif // !NDEBUG
}

TEST(NMVec, WorksWithStdRangesAlgorithms)
{
  NMVec<int> nm;
  std::vector<int> baseline = {9, 1, 7, 3, 5, 2, 8, 6, 4, 0};

  for (int v : baseline)
    nm.emplace_back(v);

  std::vector<int> sorted = baseline;
  std::ranges::sort(sorted);
  std::ranges::sort(nm);
  EXPECT_TRUE(std::ranges::equal(nm, sorted));

  auto lb = std::ranges::lower_bound(nm, 5);
  ASSERT_NE(lb, nm.end());
  EXPECT_EQ(*lb, 5);

  std::ranges::reverse(nm);
  std::ranges::reverse(sorted);
  EXPECT_TRUE(std::ranges::equal(nm, sorted));

  std::vector<int> copy;
  std::ranges::copy(nm, std::back_inserter(copy));
  EXPECT_TRUE(std::ranges::equal(copy, sorted));

  std::ranges::fill(nm, 3);
  for (std::size_t i = 0; i < nm.size(); ++i)
    EXPECT_EQ(nm[i], 3);
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

// Readers that observe an index via size() must also observe the
// construction of the element at that index (size() must synchronize with
// emplace_back's release of the size lock).  Regression test for a data
// race where size(), after observing the resize lock, returned a value
// obtained from a relaxed compare_exchange failure load, dropping the
// happens-before edge with the concurrent emplace_back.  Run under TSAN.
TEST(NMVec, ConcurrentSizeVsEmplaceBack)
{
  constexpr size_t kItems = 200000;
  constexpr size_t kReaders = 4;

  NMVec<std::unique_ptr<size_t>> v;
  std::atomic<bool> done{false};

  std::vector<std::jthread> readers;
  readers.reserve(kReaders);
  std::vector<std::atomic<bool>> ok(kReaders);
  for (size_t r = 0; r < kReaders; ++r)
    readers.emplace_back([&v, &done, &ok, r]() {
      bool all_ok = true;
      while (!done.load(std::memory_order_relaxed)) {
        size_t size = v.size();
        if (size > 0) {
          // Read both the unique_ptr object and its pointee; neither is
          // atomic, so this is only safe if size() gave us a
          // happens-before edge with the element's construction.
          size_t *p = v[size - 1].get();
          all_ok &= (p != nullptr && *p == size - 1);
        }
      }
      ok[r].store(all_ok);
    });

  for (size_t i = 0; i < kItems; ++i)
    v.emplace_back(std::make_unique<size_t>(i));

  done.store(true, std::memory_order_relaxed);
  readers.clear();

  ASSERT_EQ(v.size(), kItems);
  for (size_t r = 0; r < kReaders; ++r)
    ASSERT_TRUE(ok[r].load()) << "reader " << r << " saw a torn element";
}

TEST(NMVec, MoveAssignDestroysOldElements)
{
  CountingT<0>::reset();
  {
    NMVec<CountingT<0>> v1;
    v1.resize(5);
    EXPECT_EQ(CountingT<0>::live(), 5);

    NMVec<CountingT<0>> v2;
    v2.resize(3);
    EXPECT_EQ(CountingT<0>::live(), 8);

    v1 = std::move(v2);
    EXPECT_EQ(v1.size(), 3U);
    EXPECT_EQ(v2.size(), 0U);
    EXPECT_EQ(CountingT<0>::live(), 3);
  }
  EXPECT_EQ(CountingT<0>::live(), 0);
}
