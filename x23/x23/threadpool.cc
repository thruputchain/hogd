
#include "x23/threadpool.h"

namespace x23 {

ThreadPool::ThreadPool(size_t nthreads) : nthreads_(nthreads)
{
  auto qs = qstate_.take();
  qs->tstate_ = std::make_unique<ThreadState[]>(nthreads_);
  pool_.reserve(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i)
    pool_.push_back(std::jthread(&ThreadPool::worker, this, i));
}

ThreadPool::~ThreadPool()
{
  auto qs = qstate_.take();
  ss_.request_stop();
  qs->not_empty_.notify_all();
  qs.reset();
  pool_.clear(); // Join all threads before destroying fields
}

void
ThreadPool::drain() noexcept
{
  assert(ThreadPool::my_thread_pool != this);

  std::binary_semaphore drainwait(0);
  spawn([&drainwait] { drainwait.release(); });
  drainwait.acquire();

  auto qs = qstate_.take();
  std::vector<size_t> counts(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i)
    counts[i] = qs->tstate_[i].count_;

  for (size_t i = 0; i < nthreads_; ++i)
    while (counts[i] % 2 && counts[i] == qs->tstate_[i].count_) {
      qs->tstate_[i].please_wake_ = true;
      qs.wait(qs->count_updated_);
    }
}

void
ThreadPool::worker(size_t threadno)
{
  my_thread_pool = this;
  my_thread_no = threadno;
  size_t count = 0;
  for (;;) {
    auto qs = qstate_.take();
    auto &ts = qs->tstate_[threadno];
    ts.count_ = count++;
    if (ts.please_wake_) {
      ts.please_wake_ = false;
      qs->count_updated_.notify_all();
    }

    for (;;) {
      if (ss_.stop_requested()) [[unlikely]]
        return;
      if (!qs->q_.empty())
        break;
      qs.wait(qs->not_empty_);
    }

    std::coroutine_handle<> work = qs->q_.front();
    qs->q_.pop();
    ts.count_ = count++;
    qs.reset();

    work();
  }
}

} // namespace x23
