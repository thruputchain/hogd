
#include <algorithm>
#include <atomic>
#include <memory>

#include "x23/fastclock.h"
#include "x23/threadpool.h"

namespace x23 {

ThreadPool::ThreadPool(size_t nthreads)
  : nthreads_(nthreads), nqueues_((init_cpumaps(), llc_map->nbuckets())),
    queue_nthreads_(new std::atomic_uint[nqueues_]{}),
    runqueues_(std::make_unique<RunQueue[]>(nqueues_)),
    tstate_(std::make_unique<ThreadState[]>(nthreads))
{
  fastclock::get_ns(); // Calibrate the clock early
  workers_.reserve(nthreads_);
  threads_.reserve(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i) {
    workers_.push_back(std::make_unique<Worker>(this, i));
    threads_.emplace_back(&ThreadPool::Worker::work, workers_.back().get());
  }
}

ThreadPool::~ThreadPool()
{
  ss_.request_stop();
  for (unsigned i = 0; i < nqueues_; ++i) {
    auto ptr = runqueues_[i].take();
    runqueues_[i].qtime_ = 0;
    ptr->nonempty.notify_all();
  }
  threads_.clear(); // Join all threads before destroying fields
}

// Find and lock a runqueue for a job with cpuhint.  If the "affinity
// queue" corresponding to cpuhint's LLC is already locked, just keep
// the same queue and return.  Otherwise, pick another random queue
// and if the oldest job in the random queue is more than 10
// microseconds older than the oldest job in the affinity queue, then
// select the random queue instead of the affinity queue.  Only
// returns a queue that has at least one thread after it has been
// locked.
void
ThreadPool::qselect(ThreadPool::QPtr *lq, unsigned cpuhint)
{
  if (nqueues_ == 1) {
    relock(*lq, 0);
    return;
  }

  int affinity_queue = llc_map->bucket(cpuhint);
  if (qno(*lq) == affinity_queue)
    return;

  static thread_local uint64_t seed{
      (std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffff) |
      1UL << 20 | fastclock::get_ns() << 32};

  for (;;) {
    lq->reset();
    int other_queue = rnd_skip(seed, nqueues_, affinity_queue);
    int target_queue;

    if (queue_nthreads(affinity_queue)) {
      if (queue_nthreads(other_queue))
        target_queue = (runqueues_[affinity_queue].qtime() >
                        runqueues_[other_queue].qtime() - 10'000)
                           ? affinity_queue
                           : other_queue;
      else
        target_queue = affinity_queue;
    }
    else
      for (target_queue = other_queue; !queue_nthreads(target_queue);
           target_queue = (target_queue + 1) % nqueues_)
        ;
    *lq = runqueues_[target_queue].take();
    if (queue_nthreads(target_queue))
      return;
  }
}

void
ThreadPool::go(std::vector<Job> &&jobs)
{
  assert(!ss_.stop_requested());

  std::ranges::stable_sort(jobs, [](const Job &a, const Job &b) {
    return llc_map->bucket(a.cpuhint) < llc_map->bucket(b.cpuhint);
  });

  uint64_t now = fastclock::get_ns();

  QPtr lq;
  for (auto &j : jobs) {
    qselect(&lq, j.cpuhint);
    bool wasempty = lq->q.empty();
    lq->q.emplace_back(j, now);
    if (wasempty) {
      RunQueue::set_qtime(lq, now);
      lq->nonempty.notify_all();
    }
  }
}

void
ThreadPool::quiesce() noexcept
{
  assert(ThreadPool::my_thread_pool != this);

  std::vector<size_t> counts(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i)
    counts[i] = tstate_[i].count_;

  for (size_t i = 0; i < nthreads_; ++i)
    while (counts[i] % 2 && counts[i] == tstate_[i].count_) {
      tstate_[i].please_wake_ = true;
      tstate_[i].count_.wait(counts[i]);
    }
}

void
ThreadPool::drain() noexcept
{
  assert(ThreadPool::my_thread_pool != this);

  // Get the timestamp of the newest (back) job in each runqueue
  std::vector<size_t> backtimes(nqueues_);
  for (size_t i = 0; i < nqueues_; ++i) {
    auto ptr = runqueues_[i].take();
    backtimes[i] = ptr->q.empty() ? kEmpty : ptr->q.back().qtime;
  }

  // Loop until the oldest (front) job in each runqueue is newer than
  // the newest job in that queue at the time we started the drain.
  for (size_t i = 0; i < nqueues_; ++i)
    if (backtimes[i] != kEmpty)
      for (;;) {
        auto ptr = runqueues_[i].take();
        if (ptr->q.empty() || ptr->q.front().qtime > backtimes[i])
          break;
        ptr->need_consumed_signal = true;
        ptr.wait(ptr->consumed);
      }

  // Now also wait for all running threads to yield, since some of
  // those might still have been enqueued before drain was called.
  quiesce();
}

ThreadPool::Worker::Worker(ThreadPool *pool, size_t threadno) noexcept
  : pool_(pool), threadno_(threadno),
    rndseed_(fastclock::get_ns() << 21 | (threadno & 0xfffff))
{}

void
ThreadPool::Worker::lock_affinity_queue(QPtr &ptr) noexcept
{
  Defer _relock{[this, &ptr] { pool_->relock(ptr, bucket_); }};

  cpu_ = cpuid();
  auto newbucket = llc_map->bucket(cpu_);
  if (newbucket == bucket_)
    return;

  unsigned zero = 0;
  // The last thread in a bucket can only leave if the queue is empty
  if (pool_->queue_nthreads_[bucket_].fetch_sub(1) > 1 ||
      (pool_->relock(ptr, bucket_), ptr->q.empty()) ||
      // Oops the queue is not empty after we set the thread count to
      // zero.  We have to increase the thread count again to keep
      // servicing the queue.  But if we fail to increase the counter,
      // then it means another thread already did so, meaning it's
      // okay to leave the queue after all.
      !pool_->queue_nthreads_[bucket_].compare_exchange_strong(zero, 1))
    pool_->queue_nthreads_[bucket_ = newbucket].fetch_add(
        1, std::memory_order_relaxed);
}

ThreadPool::QPtr
ThreadPool::Worker::getq() noexcept
{
  using std::chrono::microseconds;
  static constexpr auto kTimeouts = std::array{
      microseconds(10),
      microseconds(100),
      microseconds(1000),
  };
  const auto nqueues = pool_->nqueues_;

  QPtr ptr;
  for (unsigned count = 0;;) {
    lock_affinity_queue(ptr);
    if (!ptr->q.empty())
      return ptr;
    if (pool_->ss_.stop_requested())
      return {};
    if (count >= kTimeouts.size() || nqueues == 1) {
      ptr.wait(ptr->nonempty);
      continue;
    }

    // Wait a little bit, and if we timeout try work stealing
    auto delay = kTimeouts[count++];
    if (delay.count() != 0 && ptr.with_lock([&ptr, delay](auto &ul) {
          return ptr->nonempty.wait_for(ul, delay);
        }) == std::cv_status::no_timeout)
      continue;

    // Pick two random other runqueues
    std::array<int, 2> qn;
    if (nqueues > 3)
      for (int &n : qn)
        n = rnd_skip(rndseed_, nqueues, bucket_);
    else
      for (size_t i = 0; i < qn.size(); ++i)
        qn[i] = (bucket_ + i + 1) % nqueues;

    // Find queue with older job at front
    std::array<uint64_t, qn.size()> qt;
    for (size_t i = 0; i < qn.size(); ++i)
      qt[i] = pool_->runqueues_[qn[i]].qtime();
    auto i = unsigned{qt[1] < qt[0]};

    // Don't bother with mutex if probably empty
    if (qt[i] != kEmpty) {
      pool_->relock(ptr, qn[i]);
      if (!ptr->q.empty())
        return ptr;
    }
  }
}

std::coroutine_handle<>
ThreadPool::Worker::qpop(QPtr &ptr)
{
  assert(!ptr->q.empty());

  const auto mycore = core_map->bucket(cpu_);
  const auto qi = ptr->q.front();
  const auto qt = qi.qtime;

  if (core_map->bucket(qi.job.cpuhint) != mycore) {
    // Try to find work on the same core without going forward more
    // than 5 microseconds from the oldest job.
    for (auto it = ptr->q.begin() + 1;
         it != ptr->q.end() && it->qtime < qt + 5'000; ++it)
      if (core_map->bucket(it->job.cpuhint) == mycore) {
        auto ret = it->job.value;
        ptr->q.erase(it);
        return ret;
      }
  }

  ptr->q.pop_front();
  bool empty = ptr->q.empty();
  if (empty)
    RunQueue::set_qtime(ptr, kEmpty);
  if (ptr->need_consumed_signal && (empty || ptr->q.front().qtime > qt)) {
    ptr->need_consumed_signal = false;
    ptr->consumed.notify_all();
  }

  return qi.job.value;
}

void
ThreadPool::Worker::work()
{
  my_thread_pool = pool_;
  my_thread_no = threadno_;
  cpu_ = cpuid();
  bucket_ = llc_map->bucket(cpu_);
  pool_->queue_nthreads_[bucket_]++;

  auto &ts = pool_->tstate_[threadno_];
  size_t count = 0;
  for (;;) {
    ts.count_.store(count++);
    if (ts.please_wake_) {
      ts.please_wake_ = false;
      ts.count_.notify_all();
    }

    auto ptr = getq();
    if (!ptr)
      return;
    auto task = qpop(ptr);
    ptr.reset();

    ts.count_ = count++;
    task();
  }
}

} // namespace x23
