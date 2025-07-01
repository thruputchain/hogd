
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <print>
#include <regex>
#include <unordered_map>

#include "x23/fastclock.h"
#include "x23/fileio.h"
#include "x23/threadpool.h"

namespace x23 {

ThreadPool::ThreadPool(size_t nthreads)
  : nthreads_(nthreads), nqueues_(llc_map.nbuckets_),
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
// and if the tail of the affinity queue is at least 10 microseconds
// older than the random queue, pick the random queue instead.  Only
// return queues that have at least one thread after they are locked.
void
ThreadPool::qselect(ThreadPool::QPtr *lq, unsigned cpuhint)
{
  if (nqueues_ == 1) {
    if (!*lq)
      *lq = runqueues_[0].take();
    return;
  }

  int affinity_queue = llc_map.bucket(cpuhint);
  if (qno(*lq) == affinity_queue)
    return;

  static thread_local uint64_t seed{
      (std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffff) |
      1UL << 20 | fastclock::get_ns() << 32};

  for (;;) {
    lq->reset();
    int other_queue = rnd(seed) % (nqueues_ - 1);
    other_queue += int{other_queue >= affinity_queue};
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
    return llc_map.bucket(a.cpuhint) < llc_map.bucket(b.cpuhint);
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
ThreadPool::Worker::checkcpu() noexcept
{
  cpu_ = cpuid();
  auto newbucket = llc_map.bucket(cpu_);
  if (newbucket == bucket_)
    return;

  // The last thread in a bucket can't leave if the queue is non-empty
  unsigned zero = 0;
  if (pool_->queue_nthreads_[bucket_].fetch_sub(1) > 1 ||
      pool_->runqueues_[bucket_]->q.empty() ||
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
  for (;;) {
    checkcpu();
    auto ptr = pool_->runqueues_[bucket_].take();
    if (!ptr->q.empty())
      return ptr;

    // Our queue is empty? Pick older job from two random runqueues
    if (auto nb = pool_->nqueues_; nb > 1) {
      int qns[2];
      if (nb > 3) {
        qns[0] = rnd(rndseed_) % nb;
        qns[1] = rnd(rndseed_) % nb;
      }
      else {
        qns[0] = (bucket_ + 1) % nb;
        qns[1] = (bucket_ + 2) % nb;
      }

      uint64_t qt[2]{pool_->runqueues_[qns[0]].qtime(),
		     pool_->runqueues_[qns[1]].qtime()};
      auto i = unsigned{qt[1] < qt[0]};
      // Don't bother with mutex if probably empty
      if (qt[i] != kEmpty) {
	ptr.reset();
	ptr = pool_->runqueues_[qns[i]].take();
	if (!ptr->q.empty())
	  return ptr;
	continue;
      }
    }

    if (pool_->ss_.stop_requested()) {
      assert(ptr->q.empty());
      return {};
    }
    ptr.wait(ptr->nonempty);
  }
}

std::coroutine_handle<>
ThreadPool::Worker::qpop(QPtr &ptr)
{
  assert(!ptr->q.empty());

  const auto mycore = core_map.bucket(cpu_);
  const auto qi = ptr->q.front();
  const auto qt = qi.qtime;

  if (core_map.bucket(qi.job.cpuhint) != mycore) {
    // Try to find work on the same core without going forward more
    // than 5 microseconds from the oldest job.
    for (auto it = ptr->q.begin() + 1;
         it != ptr->q.end() && it->qtime < qt + 5'000; ++it)
      if (core_map.bucket(it->job.cpuhint) == mycore) {
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
  bucket_ = llc_map.bucket(cpu_);
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

unsigned
nproc()
{
  constexpr std::array<int, 256> hex_nibble_popcount = []() consteval {
    std::array<int, 256> counts{};
    for (unsigned c = '0'; c <= '9'; ++c)
      counts[c] = std::popcount(c - '0');
    for (unsigned c = 'A'; c <= 'F'; ++c)
      counts[c] = counts[c | 0x20] = std::popcount(c - 'A' + 10);
    return counts;
  }();
  static const std::regex allowed_rx(R"(^Cpus_allowed:\s+([0-9a-f,]+)$)",
                                     std::regex::multiline);

  std::smatch matches;
  if (auto allowed = readfile<std::string>("/proc/self/status");
      allowed && regex_search(*allowed, matches, allowed_rx)) {
    unsigned sum = 0;
    for (char c : matches.str(1))
      sum += hex_nibble_popcount[c];
    if (sum > 0)
      return sum;
  }

  return std::thread::hardware_concurrency();
}

#if HAVE_SCHED_GETCPU
CpuMap
make_cpu_map(std::string_view path)
try {
  // The buckets are unique numbers but not always contiguous, so map
  // them to contiguous range of integers in [0,N).
  int nextbucket = 0;
  std::unordered_map<std::string, int> bucketmap;
  auto bucket = [&](std::string b) {
    if (auto [it, ok] = bucketmap.try_emplace(b, nextbucket); !ok)
      return it->second;
    return nextbucket++;
  };

  Vec<int> ret;

  for (const auto &cpudir :
       std::filesystem::directory_iterator("/sys/devices/system/cpu")) {
    std::string name = cpudir.path().filename();
    if (!name.starts_with("cpu"))
      continue;
    int cpu;
    try {
      cpu = stoi(name.substr(3));
    } catch (...) {
      continue;
    }
    if (unsigned(cpu) >= ret.size())
      ret.resize(cpu + 1, -1);

    std::filesystem::path idpath = cpudir.path() / path;
    auto idstr = readfile<std::string>(idpath);
    if (!idstr) {
      std::println(stderr, "{}: {}", idpath.string(), idstr.error());
      // This probably means that we are on a mobile CPU with some
      // "efficiency cores" missing an L3 cache.  That's not the kind
      // of high-core-count or multi-socket machine where L3-cache
      // affinity really helps, so just return a single bucket.
      return {{0}};
    }
    ret[cpu] = bucket(*idstr);
  }
  return ret;
} catch (...) {
  return {{0}};
}

CpuMap
make_llc_map()
try {
  int last = -1;
  for (const auto &cachedir : std::filesystem::directory_iterator(
           "/sys/devices/system/cpu/cpu0/cache")) {
    std::string name = cachedir.path().filename();
    if (!name.starts_with("index"))
      continue;
    int index;
    try {
      index = stoi(name.substr(5));
    } catch (...) {
      continue;
    }
    last = std::max(last, index);
  }

  if (last >= 0)
    return make_cpu_map(std::format("cache/index{}/shared_cpu_map", last));
  return {{0}};
} catch (...) {
  return {{0}};
}

#else  // !HAVE_SCHED_GETCPU
CpuMap
make_cpu_map(std::string_view path)
{
  return {{0}};
}
CpuMap
make_llc_map()
{
  return {{0}};
}
#endif // !HAVE_SCHED_GETCPU

} // namespace x23
