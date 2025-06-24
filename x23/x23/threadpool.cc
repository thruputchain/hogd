
#include <algorithm>
#include <filesystem>
#include <regex>
#include <unordered_map>

#include "x23/fastclock.h"
#include "x23/fileio.h"
#include "x23/threadpool.h"

#include <print> // XXX

namespace x23 {

ThreadPool::ThreadPool(size_t nthreads)
  : nthreads_(nthreads), nbuckets_(llc_map.nbuckets_),
    bucket_nthreads_(new std::atomic_uint[nbuckets_]{}),
    runqueues_(std::make_unique<Queue[]>(nthreads)),
    tstate_(std::make_unique<ThreadState[]>(nthreads))
{
  pool_.reserve(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i)
    pool_.push_back(std::make_unique<Worker>(this, i));
}

ThreadPool::~ThreadPool()
{
  ss_.request_stop();
  for (unsigned i = 0; i < nbuckets_; ++i) {
    auto ptr = runqueues_[i].q_.take();
    runqueues_[i].qtime_ = 0;
    runqueues_[i].qtime_.notify_all();
  }
  pool_.clear(); // Join all threads before destroying fields
}

void
ThreadPool::go(const std::vector<Job> &jobs)
{
  assert(!ss_.stop_requested());

  struct Q {
    bool needed{};
    uint64_t qtime{};
    QPtr ptr;
  };
  std::vector<Q> qs(nbuckets_);
  std::vector<unsigned> qnos; // indices i at which qs[i].ptr is locked
  std::vector<unsigned> wake; // buckets that need to be notified

  auto lock = [&](unsigned i) {
    if (!qs[i].ptr && bucket_nthreads_[i]) {
      qs[i].ptr = runqueues_[i].q_.take();
      if (!bucket_nthreads_[i])
        qs[i].ptr = nullptr;
      else {
        qnos.push_back(i);
        qs[i].qtime = runqueues_[i].qtime_.load(std::memory_order_relaxed);
      }
    }
  };

  for (const auto &j : jobs)
    qs[llc_map.bucket(j.cpuhint)].needed = true;
  // add an extra bucket to even out the load
  auto extra = rr_qno_.fetch_add(1, std::memory_order_relaxed) % nbuckets_;
  qs[extra].needed = true;

  for (unsigned i = 0; i < nbuckets_; ++i)
    if (qs[i].needed)
      lock(i);

  // make sure we have at least one bucket
  while (qnos.empty())
    lock((extra = (extra + 1) % nbuckets_));

  // Index of an "other queue" to check in qnos
  unsigned oq = std::ranges::lower_bound(qnos, extra) - qnos.begin();

  uint64_t now = fastclock::get_ns();

  for (const auto &j : jobs) {
    auto bucket = llc_map.bucket(j.cpuhint);
    if (oq >= qnos.size())
      oq = 0;
    if (auto altbucket = qnos[oq];
        !qs[bucket].ptr || qs[bucket].qtime < qs[altbucket].qtime - 100'000) {
      bucket = altbucket;
      ++oq;
    }
    if (qs[bucket].ptr->empty()) {
      qs[bucket].qtime = now;
      runqueues_[bucket].qtime_.store(now, std::memory_order_relaxed);
      wake.push_back(bucket);
    }
    qs[bucket].ptr->emplace_back(j, now);
  }

  qs.clear(); // release locks before waking threads
  for (auto n : wake)
    // Unfortunately have to notify_all, since we might enqueue more
    // while the queue is not empty and hence never signal again, so
    // notify_one is not sufficient.  But notify_all will be wasteful
    // precisely when the system is not loaded, which is okay.
    runqueues_[n].qtime_.notify_all();
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

  std::vector<size_t> backtimes(nthreads_);
  for (size_t i = 0; i < nthreads_; ++i) {
    auto ptr = runqueues_[i].q_.take();
    backtimes[i] = ptr->empty() ? kEmpty : ptr->back().qtime;
  }

  for (size_t i = 0; i < nthreads_; ++i)
    if (backtimes[i] != kEmpty)
      for (;;) {
	uint64_t qt = runqueues_[i].qtime_;
        if (qt > backtimes[i])
	  break;
	runqueues_[i].wake_on_qtime_increase_ = true;
	runqueues_[i].qtime_.wait(qt);
      }

  quiesce();
}

ThreadPool::Worker::Worker(ThreadPool *pool, size_t threadno)
  : pool_(pool), threadno_(threadno),
    rndseed_(fastclock::get_ns() << 16 | (threadno ? threadno : 0xffff))
{
  my_thread_pool = pool_;
  my_thread_no = threadno;
  auto cpu = cpuid();
  auto bucket = llc_map.bucket(cpu);
  pool_->bucket_nthreads_[bucket]++;
  thread_ = std::jthread(&ThreadPool::Worker::work, this);
}

void
ThreadPool::Worker::checkcpu() noexcept
{
  cpu_ = cpuid();
  auto newbucket = llc_map.bucket(cpu_);
  if (newbucket == bucket_)
    return;

  // The last thread in a bucket can't leave if the queue is non-empty
  unsigned zero = 0;
  if (pool_->bucket_nthreads_[bucket_].fetch_sub(1) > 1 ||
      pool_->runqueues_[bucket_].q_->empty() ||
      !pool_->bucket_nthreads_[bucket_].compare_exchange_strong(zero, 1))
    pool_->bucket_nthreads_[bucket_ = newbucket].fetch_add(
        1, std::memory_order_relaxed);
}

ThreadPool::Worker::LockedQueue
ThreadPool::Worker::getq() noexcept
{
  for (;;) {
    checkcpu();
    auto &myq = pool_->runqueues_[bucket_];
    if (auto ptr = myq.q_.take(); !ptr->empty())
      return {.qp = &myq, .ptr = std::move(ptr)};

    // Our queue is empty? Pick older job from two run random runqueues
    if (auto nb = pool_->nbuckets_; nb > 1) {
      Queue *qs[2];
      if (nb > 3) {
        qs[0] = &pool_->runqueues_[rnd(rndseed_) % pool_->nbuckets_];
        qs[1] = &pool_->runqueues_[rnd(rndseed_) % pool_->nbuckets_];
      }
      else {
        qs[0] = &pool_->runqueues_[(bucket_ + 1) % nb];
        qs[1] = &pool_->runqueues_[(bucket_ + 2) % nb];
      }

      uint64_t qt[2]{qs[0]->qtime(), qs[1]->qtime()};
      auto i = unsigned{qt[1] < qt[0]};
      if (qt[i] != kEmpty) // Don't bother with mutex if probably empty
        if (auto ptr = qs[i]->q_.take(); !ptr->empty())
          return {.qp = qs[i], .ptr = std::move(ptr)};
    }

    if (pool_->ss_.stop_requested())
      return {};
    myq.qtime_.wait(kEmpty);
  }
}

std::coroutine_handle<>
ThreadPool::Worker::qpop(LockedQueue &lq)
{
  auto mycore = core_map.bucket(cpu_);
  const auto qi = lq.ptr->front();
  const auto qt = lq.ptr->front().qtime;

  if (core_map.bucket(qi.job.cpuhint) != mycore) {
    // Try to find work on the same core without going forward more
    // than 5 microseconds.
    for (auto it = lq.ptr->begin() + 1;
         it != lq.ptr->end() && it->qtime < qt + 5'000; ++it)
      if (core_map.bucket(it->job.cpuhint) != mycore) {
	auto ret = it->job.value;
	lq.ptr->erase(it);
	return ret;
      }
  }

  lq.ptr->pop_front();

  uint64_t nqt = lq.ptr->empty() ? kEmpty : lq.ptr->front().qtime;
  if (nqt != qt) {
    lq.qp->qtime_.store(kEmpty, std::memory_order_relaxed);
    if (lq.qp->wake_on_qtime_increase_) {
      lq.qp->wake_on_qtime_increase_ = false;
      lq.qp->qtime_.notify_all();
    }
  };

  return qi.job.value;
}

void
ThreadPool::Worker::work()
{
  size_t count = 0;
  auto &ts = pool_->tstate_[threadno_];
  for (;;) {
    ts.count_.store(count++);
    if (ts.please_wake_) {
      ts.please_wake_ = false;
      ts.count_.notify_all();
    }

    auto lq = getq();
    if (!lq.qp)
      return;
    auto task = qpop(lq);

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
      continue;
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
