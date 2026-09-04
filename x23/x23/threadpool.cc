
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <csignal>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <sys/signal.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "x23/cleanup.h"
#include "x23/cpumap.h"
#include "x23/debug.h"
#include "x23/fastclock.h"
#include "x23/signal.h"
#include "x23/threadpool.h"
#include "x23/utility.h"
#include "x23/vec.h"

namespace x23 {

ThreadPool::ThreadPool(size_t nthreads)
  : nthreads_(nthreads), nqueues_((init_cpumaps(), llc_map->nbuckets())),
    queue_nthreads_(new std::atomic_uint[nqueues_]{}),
    runqueues_(std::make_unique<RunQueue[]>(nqueues_)),
    tstate_(std::make_unique<ThreadState[]>(nthreads)),
    qcbq_service_thread_(&ThreadPool::qcbq_service, this)
{
  fastclock::get_ns(); // Calibrate the clock early
  workers_.reserve(nthreads_);
  threads_.reserve(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i) {
    workers_.push_back(std::make_unique<Worker>(this, i));
    threads_.emplace_back(&ThreadPool::Worker::work, workers_.back().get());
  }
  sigcb_ = signal_cb(SIGUSR1, [this] { debug_dump(); });
}

ThreadPool::~ThreadPool()
{
  // Wait for the detached threads async() spawned.  Such a thread ends
  // in make_callback(), which resumes the awaiting coroutine by handing
  // it to a worker from inside go() -- and go() then goes on to call
  // notify_all() on the runqueue's condition variable.  The resumed
  // coroutine, and whatever destroys the pool once it completes, race
  // with that tail: TSAN reports the destructor's pthread_cond_destroy
  // against the detached thread's still-running pthread_cond_broadcast.
  //
  // The wait is a poll rather than nasync_.wait().  A detached thread
  // that woke us through the atomic would have to touch nasync_ *after*
  // the decrement that lets this destructor proceed, which is the same
  // use-after-free one level down.  Keeping the decrement as the
  // thread's last access to the pool avoids that, and the expected wait
  // is the tail of one go() call, so the spin is not a real cost.
  //
  // This precedes request_stop() so that a thread on its way into
  // make_callback() does not trip go()'s assertion that the pool is
  // still running.
  //
  // A task must not own the pool it runs on.  The count covers a task
  // until everything it captured has been destroyed, so if destroying
  // those captures is what drops the pool's last reference, we are
  // being run from a task whose own count cannot go away until we
  // return, and the wait below would never finish.  Destroying a pool
  // from inside its own async() task is unsupported for the same
  // reason drain() and quiesce() refuse to run in a pool thread, and
  // is caught the same way.
  x23assert(my_async_pool != this);
  while (nasync_.load(std::memory_order_acquire))
    std::this_thread::yield();

  ss_.request_stop();
  for (unsigned i = 0; i < nqueues_; ++i) {
    auto ptr = runqueues_[i].take();
    runqueues_[i].qtime_ = 0;
    ptr->nonempty.notify_all();
  }
  threads_.clear(); // Join all threads before destroying fields

  std::unique_lock lk{qcbq_lock_};
  qcbq_nonempty_.notify_all();
  // qcbq_service might exit after ss_.request_stop(), so purge final
  // quiescent callbacks after workers have exited.
  qcbq_do_service(lk);
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
  x23assert(!ss_.stop_requested());

  std::ranges::stable_sort(jobs, [](const Job &a, const Job &b) {
    return llc_map->bucket(a.cpuhint) < llc_map->bucket(b.cpuhint);
  });

  uint64_t now = fastclock::get_ns();
  QPtr lq;
  std::condition_variable *nonempty = nullptr;
  for (auto &j : jobs) {
    int affinity_queue = llc_map->bucket(j.cpuhint);
    if (nonempty && qno(lq) != affinity_queue) {
      lq.reset();
      nonempty->notify_all();
      nonempty = nullptr;
    }
    if (enqueue_selected(lq, j, now))
      nonempty = &lq->nonempty;
  }
  lq.reset();
  if (nonempty)
    nonempty->notify_all();
}

void
ThreadPool::go(Job job)
{
  x23assert(!ss_.stop_requested());

  uint64_t now = fastclock::get_ns();
  QPtr lq;
  bool wasempty = enqueue_selected(lq, job, now);
  auto *nonempty = &lq->nonempty;
  lq.reset();
  if (wasempty)
    nonempty->notify_all();
}

void
ThreadPool::quiesce() noexcept
{
  x23assert(ThreadPool::my_thread_pool != this);

  std::vector<size_t> counts(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i)
    counts[i] =
        quiesce_count(tstate_[i].count_.load(std::memory_order_acquire));

  for (size_t i = 0; i < nthreads_; ++i) {
    while (counts[i] % 2) {
      size_t current =
          tstate_[i].count_.fetch_or(kQuiesceWake, std::memory_order_acq_rel);
      if (quiesce_count(current) != counts[i])
        break;
      tstate_[i].count_.wait(current | kQuiesceWake, std::memory_order_relaxed);
    }
  }
}

void
ThreadPool::drain() noexcept
{
  x23assert(ThreadPool::my_thread_pool != this);

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

void
ThreadPool::qcbq_service()
{
  std::unique_lock lk(qcbq_lock_);
  for (;;) {
    qcbq_do_service(lk);
    if (ss_.stop_requested())
      return;
    qcbq_nonempty_.wait(lk);
  }
}

void
ThreadPool::qcbq_do_service(std::unique_lock<std::mutex> &lk)
{
  assert(lk);
  for (;;) {
    auto v = std::exchange(qcbq_, {});
    if (v.empty())
      return;
    lk.unlock();
    quiesce();
    for (auto &f : v)
      std::invoke(f);
    lk.lock();
  }
}

std::string
ThreadPool::show()
{
  std::vector<uint64_t> oldest(nqueues_);
  std::vector<unsigned int> nt(nqueues_);
  uint64_t now = fastclock::get_ns();
  for (unsigned int i = 0; i < nqueues_; ++i) {
    oldest[i] = runqueues_[i].qtime();
    nt[i] = queue_nthreads(i);
  }
  std::string ret =
      std::format("ThreadPool {}: {} threads, {} run queues, {} ns\n",
                  (void *)this, nthreads_, nqueues_, now);
  for (unsigned int i = 0; i < nqueues_; ++i) {
    uformat_to(&ret, "  queue {}: {} threads", i, nt[i]);
    if (oldest[i] == kEmpty)
      ret += ", EMPTY";
    else
      uformat_to(&ret, ", oldest job age {:.3f} us (time {})",
                 int64_t(now - oldest[i]) / 1000.0, oldest[i]);
    ret += '\n';
  }
  return ret;
}

void
ThreadPool::debug_dump()
{
  std::string msg = show();
  int fd = debug.fd(); // NOLINT(readability-static-accessed-through-instance)
  if (fd < 0)
    fd = 2;
  ::write(fd, msg.data(), msg.size());
}

ThreadPool::Worker::Worker(ThreadPool *pool, size_t threadno) noexcept
  : pool_(pool), threadno_(threadno),
    // Zero is an absorbing state for xorshift, so ensure seed is nonzero.
    rndseed_(fastclock::get_ns() << 21 | (threadno & 0xfffff) | 1UL << 20)
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
ThreadPool::Worker::qpop(QPtr &ptr) const
{
  x23assert(!ptr->q.empty());

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
    size_t oldcount = ts.count_.exchange(count++, std::memory_order_release);
    if (oldcount & kQuiesceWake)
      ts.count_.notify_all();

    auto ptr = getq();
    if (!ptr)
      return;
    auto task = qpop(ptr);
    ts.count_.store(count++, std::memory_order_release);
    ptr.reset();
    task();
  }
}

bool
in_thread_pool()
{
  return ThreadPool::get_thread_pool() != nullptr;
}

} // namespace x23
