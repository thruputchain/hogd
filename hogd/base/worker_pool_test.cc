#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include "hogd/base/types.h"
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
  for (const auto &future : futures)
    future.wait();

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

TEST(WorkerPoolTest, SubmitDetached)
{
  constexpr int kJobs = 10'000;
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::atomic<int> count{0};

  for (int i = 0; i < kJobs; i++)
    pool.submit_detached([&count] { count++; });

  // Wait until all detached jobs complete by polling pending_jobs.
  while (pool.pending_jobs() > 0)
    ;

  EXPECT_EQ(count, kJobs);
}

TEST(WorkerPoolTest, JobScopeWaitRunsAllJobs)
{
  constexpr int kJobs = 10'000;
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::atomic<int> count{0};

  {
    WorkerPool::JobScope scope(pool);
    scope.reserve(kJobs);
    for (int i = 0; i < kJobs; i++)
      scope.submit([&count] { count++; });
    scope.wait();
    // After wait() every job has completed.
    EXPECT_EQ(count, kJobs);
  }
  EXPECT_EQ(count, kJobs);
}

TEST(WorkerPoolTest, JobScopeWaitRethrows)
{
  WorkerPool &pool = WorkerPool::get_default_pool();
  WorkerPool::JobScope scope(pool);
  scope.submit([] { throw std::invalid_argument("foo"); });
  EXPECT_THROW(scope.wait(), std::invalid_argument);
}

// Even when wait() is never called (e.g. an early return), the destructor
// must join every submitted job before the captured local leaves scope.
TEST(WorkerPoolTest, JobScopeDestructorDrainsWithoutWait)
{
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::atomic<bool> job_done{false};

  {
    WorkerPool::JobScope scope(pool);
    scope.submit([&job_done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      job_done.store(true, std::memory_order_release);
    });
    // Intentionally do not call wait(); the destructor must block until the
    // job finishes so that `job_done` (a local here) is never written after
    // this scope exits.
  }

  EXPECT_TRUE(job_done.load(std::memory_order_acquire));
}

// An exception unwinding through the scope (after some jobs are queued) must
// still drain every job before the captured locals are destroyed.
TEST(WorkerPoolTest, JobScopeDrainsWhileUnwinding)
{
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::atomic<bool> job_done{false};

  try {
    WorkerPool::JobScope scope(pool);
    scope.submit([&job_done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      job_done.store(true, std::memory_order_release);
    });
    throw std::runtime_error("unwind");
  } catch (const std::runtime_error &) {
    // The JobScope destructor ran during unwinding and joined the job.
    EXPECT_TRUE(job_done.load(std::memory_order_acquire));
  }
}

TEST(WorkerPoolTest, ParallelForEach)
{
  constexpr ssize_t kElements = 1024;
  constexpr size_t kWidth = 16;
  std::vector<int> vec(kElements, 1);
  WorkerPool &pool = WorkerPool::get_default_pool();
  std::atomic<int> count{0};
  pool.for_each_parallel(vec, [&count](const int &v) { count += v; }, kWidth);
  EXPECT_EQ(count, kElements);
}

} // namespace hogd
