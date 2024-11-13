#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>
#include <vector>

#include "hogd/base/worker_pool.h"

namespace hogd {

TEST(WorkerPoolTest, SubmitWork)
{
  constexpr int kJobs = 10'000;
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::atomic<int> count{0};
  std::vector<std::future<void>> futures(kJobs);

  // enqueue jobs
  for (auto &future : futures)
    future = pool.submit([&count] { count++; });

  // wait for completions
  for (auto &future : futures)
    future.get();

  EXPECT_EQ(count, kJobs);
}

TEST(WorkerPoolTest, AdjustWorkers)
{
  constexpr int kJobs = 10'000;
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::atomic<int> count{0};
  std::vector<std::future<void>> futures(kJobs);

  // enqueue jobs
  for (auto &future : futures)
    future = pool.submit([&count] { count++; });

  // adjust workers
  pool.adjust_workers(1);

  // wait for completions
  for (auto &future : futures)
    future.get();

  EXPECT_EQ(count, kJobs);
}

TEST(WorkerPoolTest, Exceptions)
{
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::future<void> f = pool.submit([] { throw std::invalid_argument("foo"); });
  EXPECT_THROW(f.get(), std::invalid_argument);
}

} // namespace hogd
