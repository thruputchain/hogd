
#include <filesystem>
#include <regex>
#include <unordered_map>

#include "x23/fileio.h"
#include "x23/threadpool.h"

#include <print> // XXX

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
    if (cpu >= ret.size())
      ret.resize(cpu+1, -1);

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
#else // !HAVE_SCHED_GETCPU
CpuMap
make_cpu_map(std::string_view path)
{
  return {{0}};
}
#endif // !HAVE_SCHED_GETCPU

} // namespace x23
