#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <x23/threadpool.h>

#include "hogd/base/coropoll.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/init.h"

namespace hogd {

struct TruncateHasher {
  uint64_t operator()(std::span<const uint8_t> key) const
  {
    uint64_t hash = 0;
    std::memcpy(&hash, key.data(), std::min(key.size(), sizeof(hash)));
    return std::byteswap(hash);
  }
};

TEST(ConcurrentMap, ValueBucketUsage)
{
  using bucket_t = detail::ValueBucket<std::vector<uint8_t>, int>;

  std::atomic<bucket_t *> value_bucket_ptr = nullptr;
  BumpAllocator<bucket_t> allocator;

  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2, 3},
                   4);

  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2}, 5);
  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1}, 6);

  // double inserts don't change the data structure
  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2}, 7);
  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2, 3},
                   8);

  ASSERT_NE(value_bucket_ptr.load(), nullptr);
  ASSERT_EQ(value_bucket_ptr.load()->value_, 4);
  ASSERT_NE(value_bucket_ptr.load()->next_ptr_.load(), nullptr);
  ASSERT_EQ(value_bucket_ptr.load()->next_ptr_.load()->value_, 5);
  ASSERT_NE(value_bucket_ptr.load()->next_ptr_.load()->next_ptr_.load(),
            nullptr);
  ASSERT_EQ(value_bucket_ptr.load()->next_ptr_.load()->next_ptr_.load()->value_,
            6);
  ASSERT_EQ(value_bucket_ptr.load()
                ->next_ptr_.load()
                ->next_ptr_.load()
                ->next_ptr_.load(),
            nullptr);
  bucket_t::clear(value_bucket_ptr.load());
}

TEST(ConcurrentMap, DestructorLeakTest)
{
  ConcurrentMap<std::vector<uint8_t>, std::unique_ptr<int>, TruncateHasher> map;

  auto *value1 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                   std::make_unique<int>(4));
  ASSERT_EQ(**value1, 4);
  auto *value2 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(5));
  ASSERT_EQ(**value2, 5);
  auto *value3 =
      map.insert_or_get(std::vector<uint8_t>{1}, std::make_unique<int>(6));
  ASSERT_EQ(**value3, 6);
  auto *value4 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(7));
  ASSERT_EQ(**value4, 5);
  auto *value5 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                   std::make_unique<int>(8));
  ASSERT_EQ(**value5, 4);

  EXPECT_EQ(value1, value5);
  EXPECT_EQ(value2, value4);

  // make sure that duplicate inserts don't get leaked
  auto *value6 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                   std::make_unique<int>(4));
  ASSERT_EQ(**value6, 4);
  auto *value7 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(5));
  ASSERT_EQ(**value7, 5);
  auto *value8 =
      map.insert_or_get(std::vector<uint8_t>{1}, std::make_unique<int>(6));
  ASSERT_EQ(**value8, 6);
  auto *value9 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(7));
  ASSERT_EQ(**value9, 5);
  auto *value10 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                    std::make_unique<int>(8));
  ASSERT_EQ(**value10, 4);

  EXPECT_EQ(value6, value10);
  EXPECT_EQ(value7, value9);

  EXPECT_EQ(value1, value6);
  EXPECT_EQ(value2, value7);
  EXPECT_EQ(value3, value8);
  EXPECT_EQ(value4, value9);
  EXPECT_EQ(value5, value10);
}

TEST(ConcurrentMap, ConcurrentInsert)
{
  // This is an imperfect test to check that if multiple threads attempt to
  // allocate bucket entries for the same entry, only one gets inserted, and the
  // rest get properly destroyed.
  ConcurrentMap<std::vector<uint8_t>, int, TruncateHasher> map;

  auto &pool = WorkerPool::get_default_pool();
  pool.for_each_parallel(
      std::ranges::iota_view(0, 128),
      [&map](int i) {
        uint8_t lastbit = i;
        std::vector<uint8_t> key = {0, 0, 0, 0, 0, 0, 0, lastbit};
        map.insert_or_get(key, i);
      },
      128);
}

TEST(ConcurrentMap, InsertOrGet)
{
  EXPECT_EQ(TruncateHasher{}(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}),
            0x0000'0000'0000'0001);

  ConcurrentMap<std::vector<uint8_t>, int, TruncateHasher> map;
  int *value =
      map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}, 4);
  ASSERT_EQ(*value, 4);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}, 5);
  ASSERT_EQ(*value, 4);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 2}, 6);
  ASSERT_EQ(*value, 6);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 3}, 7);
  ASSERT_EQ(*value, 7);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 4}, 8);
  ASSERT_EQ(*value, 8);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 1, 0}, 9);
  ASSERT_EQ(*value, 9);
}

TEST(ConcurrentMap, ForEachParallel)
{
  ConcurrentMap<std::vector<uint8_t>, int, TruncateHasher> map;
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}, 4);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 2}, 5);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 3}, 6);
  // separate bucket
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 1, 0, 0, 0, 4}, 7);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 1, 0, 0, 0, 5}, 8);

  std::mutex mtx;
  std::set<int> values;
  map.for_each_parallel(
      [&values, &mtx](std::pair<const std::vector<uint8_t> &, int &> value) {
        std::scoped_lock lock(mtx);
        values.insert(value.second);
      });

  ASSERT_EQ(values.size(), 5);
  ASSERT_TRUE(values.contains(4));
  ASSERT_TRUE(values.contains(5));
  ASSERT_TRUE(values.contains(6));
  ASSERT_TRUE(values.contains(7));
  ASSERT_TRUE(values.contains(8));
}

TEST(ConcurrentMap, ForEachParallelDestructive)
{
  ConcurrentMap<std::vector<uint8_t>, int, TruncateHasher> map;
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}, 4);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 2}, 5);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 3}, 6);
  // separate bucket
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 1, 0, 0, 0, 4}, 7);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 1, 0, 0, 0, 5}, 8);

  std::mutex mtx;
  std::set<int> values;
  map.for_each_parallel_destructive(
      [&values, &mtx](std::pair<const std::vector<uint8_t> &, int &> value) {
        std::scoped_lock lock(mtx);
        values.insert(value.second);
      });

  ASSERT_EQ(values.size(), 5);
  ASSERT_TRUE(values.contains(4));
  ASSERT_TRUE(values.contains(5));
  ASSERT_TRUE(values.contains(6));
  ASSERT_TRUE(values.contains(7));
  ASSERT_TRUE(values.contains(8));
}

struct TsanPatternValue {
  explicit TsanPatternValue(uint64_t seed) : seed_(seed), inverse_(~seed)
  {
    for (size_t i = 0; i < words_.size(); ++i) {
      words_[i] = seed ^ (0x9e37'79b9'7f4a'7c15ULL + i);
    }
  }

  bool valid() const
  {
    if (inverse_ != ~seed_) {
      return false;
    }
    for (size_t i = 0; i < words_.size(); ++i) {
      if (words_[i] != (seed_ ^ (0x9e37'79b9'7f4a'7c15ULL + i))) {
        return false;
      }
    }
    return true;
  }

  uint64_t seed_;
  uint64_t inverse_;
  std::array<uint64_t, 16> words_;
};

TEST(ConcurrentMap, InsertOrGetMissingAcquireTsanRace)
{
  constexpr size_t kThreads = 16;
  constexpr size_t kRounds = 64;

  std::atomic<bool> observed_invalid = false;
  for (size_t round = 0; round < kRounds; ++round) {
    ConcurrentMap<uint64_t, TsanPatternValue> map(1024);
    std::atomic<size_t> ready = 0;
    std::atomic<bool> start = false;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (size_t thread_id = 0; thread_id < kThreads; ++thread_id) {
      threads.emplace_back([&, thread_id] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        TsanPatternValue *value = map.insert_or_get(0x1234'5678'9abc'def0ULL,
                                                    (round << 8) | thread_id);
        if (!value->valid()) {
          observed_invalid.store(true, std::memory_order_relaxed);
        }
      });
    }

    while (ready.load(std::memory_order_acquire) != kThreads) {
      std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto &thread : threads) {
      thread.join();
    }
  }

  EXPECT_FALSE(observed_invalid.load(std::memory_order_relaxed));
}

namespace {

std::atomic<int> destroy_count{0};

struct DestroyCounter {
  int id;

  explicit DestroyCounter(int i) : id(i) {}
  ~DestroyCounter() { destroy_count.fetch_add(1, std::memory_order_relaxed); }
};

struct IntHasher {
  uint64_t operator()(int key) const
  {
    return static_cast<uint64_t>(static_cast<unsigned>(key));
  }
};

} // namespace

TEST(ConcurrentMap, DestructiveIterationDoubleDestroyOnException)
{

  destroy_count.store(0, std::memory_order_relaxed);

  constexpr int kNumEntries = 4;
  constexpr int kThrowAfter = 2;

  {
    ConcurrentMap<int, DestroyCounter, IntHasher> map(16);

    for (int i = 0; i < kNumEntries; i++) {
      map.insert_or_get(i, i);
    }

    std::atomic<int> callback_count{0};
    EXPECT_THROW(map.for_each_parallel_destructive(
                     [&](std::pair<const int &, DestroyCounter &>) {
                       if (callback_count.fetch_add(
                               1, std::memory_order_relaxed) >= kThrowAfter) {
                         throw std::runtime_error(
                             "simulated exception in destructive callback");
                       }
                     }),
                 std::runtime_error);
  }

  EXPECT_EQ(destroy_count.load(std::memory_order_relaxed), kNumEntries);
}

namespace {

// Distinct value types give each test its own pool, since the pool is per
// ConcurrentMap instantiation.
struct RecycleAfterDestructive {
  int v;
};
struct RecycleAfterSlowClear {
  int v;
  explicit RecycleAfterSlowClear(int i) : v(i) {}
  ~RecycleAfterSlowClear()
  {
    destroy_count.fetch_add(1, std::memory_order_relaxed);
  }
};
struct RecycleBounded {
  int v;
};
struct RecycleStaticOwner {
  int v;
};

// Counts bucket-array allocations so the tests below can tell reuse from a
// fresh allocation.
std::atomic<size_t> bucket_array_allocs{0};
std::atomic<size_t> bucket_array_frees{0};

template<typename T> struct CountingBucketAllocator {
  using value_type = T;
  // Stateless, like std::allocator: containers may steal each other's
  // buffers instead of moving elements (std::atomic is not assignable).
  using is_always_equal = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  CountingBucketAllocator() = default;
  template<typename U>
  CountingBucketAllocator(const CountingBucketAllocator<U> & /*other*/) noexcept
  {}

  T *allocate(size_t n)
  {
    bucket_array_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::allocator<T>{}.allocate(n);
  }
  void deallocate(T *p, size_t n) noexcept
  {
    bucket_array_frees.fetch_add(1, std::memory_order_relaxed);
    std::allocator<T>{}.deallocate(p, n);
  }

  friend bool operator==(const CountingBucketAllocator & /*a*/,
                         const CountingBucketAllocator & /*b*/)
  {
    return true;
  }
};

template<typename V>
using counting_map_t =
    ConcurrentMap<int, V, IntHasher,
                  CountingBucketAllocator<std::atomic<typename ConcurrentMap<
                      int, V, IntHasher>::value_bucket_t *>>>;

struct BucketArrayCounts {
  size_t allocs = bucket_array_allocs.load(std::memory_order_relaxed);
  size_t frees = bucket_array_frees.load(std::memory_order_relaxed);

  size_t allocs_since() const
  {
    return bucket_array_allocs.load(std::memory_order_relaxed) - allocs;
  }
  size_t frees_since() const
  {
    return bucket_array_frees.load(std::memory_order_relaxed) - frees;
  }
};

} // namespace

TEST(ConcurrentMap, RecycledBucketArrayIsEmptyAfterDestructiveIteration)
{
  using map_t = counting_map_t<RecycleAfterDestructive>;
  constexpr size_t kBuckets = 64;
  constexpr int kNumEntries = 200; // more entries than buckets: chains

  BucketArrayCounts counts;
  {
    map_t map(kBuckets);
    EXPECT_EQ(counts.allocs_since(), 1U);
    for (int i = 0; i < kNumEntries; i++) {
      ASSERT_NE(map.insert_or_get(i, i), nullptr);
    }
    std::atomic<int> visited{0};
    map.for_each_parallel_destructive(
        [&](std::pair<const int &, RecycleAfterDestructive &>) {
          visited.fetch_add(1, std::memory_order_relaxed);
        });
    EXPECT_EQ(visited.load(), kNumEntries);
  }
  // The array went to the pool, not back to the allocator.
  EXPECT_EQ(counts.frees_since(), 0U);

  {
    // A same-size map reuses the pooled array and must look empty.
    map_t map(kBuckets);
    EXPECT_EQ(counts.allocs_since(), 1U);
    for (int i = 0; i < kNumEntries; i++) {
      EXPECT_EQ(map.get(i), nullptr);
    }
    for (int i = 0; i < kNumEntries; i++) {
      auto *value = map.insert_or_get(i, i + 1000);
      ASSERT_NE(value, nullptr);
      EXPECT_EQ(value->v, i + 1000);
    }
    map.clear();
  }
  EXPECT_EQ(counts.frees_since(), 0U);

  {
    // A different bucket count does not match the pooled array.
    map_t map(kBuckets * 2);
    EXPECT_EQ(counts.allocs_since(), 2U);
    for (int i = 0; i < kNumEntries; i++) {
      EXPECT_EQ(map.get(i), nullptr);
    }
    ASSERT_NE(map.insert_or_get(1, 1), nullptr);
  }
  EXPECT_EQ(counts.frees_since(), 0U);
}

TEST(ConcurrentMap, RecycledBucketArrayIsEmptyAfterSlowClear)
{
  using map_t = counting_map_t<RecycleAfterSlowClear>;
  constexpr size_t kBuckets = 16;
  constexpr int kNumEntries = 50;

  destroy_count.store(0, std::memory_order_relaxed);
  BucketArrayCounts counts;

  {
    // Destroyed without clear(): the deferred clear on the pool must both
    // destroy every value and leave the array in a recyclable (all-null) state.
    map_t map(kBuckets);
    for (int i = 0; i < kNumEntries; i++) {
      ASSERT_NE(map.insert_or_get(i, i), nullptr);
    }
  }
  EXPECT_EQ(destroy_count.load(std::memory_order_relaxed), kNumEntries);
  EXPECT_EQ(counts.allocs_since(), 1U);
  EXPECT_EQ(counts.frees_since(), 0U);

  {
    map_t map(kBuckets);
    EXPECT_EQ(counts.allocs_since(), 1U); // reused
    for (int i = 0; i < kNumEntries; i++) {
      EXPECT_EQ(map.get(i), nullptr);
    }
    ASSERT_NE(map.insert_or_get(7, 7), nullptr);
  }
  // Exactly one more value destroyed: nothing stale survived in the array.
  EXPECT_EQ(destroy_count.load(std::memory_order_relaxed), kNumEntries + 1);
}

TEST(ConcurrentMap, RecyclingPastThePoolBound)
{
  using map_t = counting_map_t<RecycleBounded>;
  constexpr size_t kBuckets = 16;
  constexpr size_t kExcess = 3;
  constexpr size_t kLiveMaps = map_t::kMaxPooledStorages + kExcess;

  BucketArrayCounts counts;
  {
    std::vector<std::unique_ptr<map_t>> maps;
    for (size_t i = 0; i < kLiveMaps; i++) {
      maps.push_back(std::make_unique<map_t>(kBuckets));
      ASSERT_NE(maps.back()->insert_or_get(1, 1), nullptr);
    }
    EXPECT_EQ(counts.allocs_since(), kLiveMaps);
  }
  // The pool keeps kMaxPooledStorages; the excess is freed.
  EXPECT_EQ(counts.frees_since(), kExcess);

  {
    // The pooled arrays are all taken before any fresh one is allocated.
    std::vector<std::unique_ptr<map_t>> maps;
    for (size_t i = 0; i < kLiveMaps; i++) {
      maps.push_back(std::make_unique<map_t>(kBuckets));
      EXPECT_EQ(maps.back()->get(1), nullptr);
    }
    EXPECT_EQ(counts.allocs_since(), kLiveMaps + kExcess);
  }
  EXPECT_EQ(counts.frees_since(), 2 * kExcess);
}

namespace {
// Set after the map destructor returns; values wait on it, so an inline clear
// is reported as a failure instead of hanging.
std::atomic<bool> map_destructor_returned{false};
std::atomic<int> values_destroyed_after_return{0};
struct WaitsForMapDestructor {
  int v;
  explicit WaitsForMapDestructor(int i) : v(i) {}
  ~WaitsForMapDestructor()
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!map_destructor_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    if (map_destructor_returned.load(std::memory_order_acquire)) {
      values_destroyed_after_return.fetch_add(1, std::memory_order_relaxed);
    }
  }
};
} // namespace

// A pool worker dropping an uncleared map must not clear its bucket array
// inline: that walk is what made e.g. closing a viewing snapshot cost ~16ms.
TEST(ConcurrentMap, WorkerDroppingUnclearedMapDefersTheClear)
{
  using map_t = ConcurrentMap<int, WaitsForMapDestructor, IntHasher>;
  ASSERT_EQ(x23::ThreadPool::get_thread_pool(), nullptr);
  map_destructor_returned.store(false, std::memory_order_relaxed);
  values_destroyed_after_return.store(0, std::memory_order_relaxed);
  default_threadpool().spawn([] {
    {
      map_t map(16);
      ASSERT_NE(map.insert_or_get(1, 1), nullptr);
    }
    map_destructor_returned.store(true, std::memory_order_release);
  });
  // The first drain finishes the job above, whose map destructor queues the
  // clear; the second drain finishes that clear.
  map_t::wait_for_background_recycling();
  map_t::wait_for_background_recycling();
  EXPECT_EQ(values_destroyed_after_return.load(std::memory_order_relaxed), 1);
}

// Fills more than one 64MB allocator region, recycles the storage, and fills
// it again: reset_all_regions() must hand the retained regions out again in
// order and every old value must be destroyed exactly once.
TEST(ConcurrentMap, RecycledStorageReusesMultipleAllocatorRegions)
{
  using map_t = counting_map_t<RecycleAfterSlowClear>;
  constexpr size_t kBuckets = 1 << 16;
  // 600k entries of a 128-byte (one cache line of key + one of value)
  // bucket need ~77MB, i.e. two 64MB regions.
  constexpr int kNumEntries = 600'000;
  static_assert(static_cast<size_t>(kNumEntries) *
                    sizeof(map_t::value_bucket_t) >
                map_t::value_allocator_t::kMmapLength);

  destroy_count.store(0, std::memory_order_relaxed);
  BucketArrayCounts counts;

  for (int round = 0; round < 3; round++) {
    {
      map_t map(kBuckets);
      WorkerPool::get_default_pool().for_each_parallel(
          std::ranges::iota_view{0, kNumEntries},
          [&map](int i) { ASSERT_NE(map.insert_or_get(i, i), nullptr); }, 4096);
      for (int i = 0; i < kNumEntries; i += 997) {
        auto *value = map.get(i);
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(value->v, i);
      }
    }
    EXPECT_EQ(destroy_count.load(std::memory_order_relaxed),
              kNumEntries * (round + 1));
    // One bucket array for all rounds: the storage was recycled each time.
    EXPECT_EQ(counts.allocs_since(), 1U);
  }
}

// The map below outlives this test: it is owned by a function-local static
// that is initialized before the pool of its map type is first used, so it is
// destroyed after that pool would have been. Returning its array during
// static teardown must not touch a destroyed pool (ASan catches the
// use-after-free if it does).
TEST(ConcurrentMap, BucketArrayPoolOutlivesStaticOwners)
{
  using map_t = ConcurrentMap<int, RecycleStaticOwner, IntHasher>;
  static std::unique_ptr<map_t> owner;
  ASSERT_EQ(owner, nullptr);
  owner = std::make_unique<map_t>(16);
  ASSERT_NE(owner->insert_or_get(1, 1), nullptr);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
