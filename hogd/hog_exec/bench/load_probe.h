#pragma once

// Where a load generator's throughput goes: how much each connection's
// requests carried and how long they took, and how much CPU each of the
// generator's own threads used.  A generator that drives several servers
// can then tell a slow server from its own limits: when a server's batch
// latency stays flat but its throughput falls, the time is being lost in
// the generator between a reply and its next request.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"

namespace hogd {

// One connection's requests: how many batches and txs it carried and their
// total round-trip time.  Updated by every coroutine on the connection.
struct alignas(kCacheLineSize) ConnectionLoad {
  std::atomic<uint64_t> batches{0};
  std::atomic<uint64_t> txs{0};
  std::atomic<uint64_t> latency_ns{0};

  void record(size_t ntxs, std::chrono::nanoseconds latency)
  {
    batches.fetch_add(1, std::memory_order_relaxed);
    txs.fetch_add(ntxs, std::memory_order_relaxed);
    latency_ns.fetch_add(static_cast<uint64_t>(latency.count()),
                         std::memory_order_relaxed);
  }
};

// CPU time this process's threads have used so far, by thread id, with each
// thread's name.  Linux only: empty where /proc/self/task is missing.
struct ThreadCpu {
  std::string name;
  uint64_t ticks = 0;
};
using ThreadCpuSnapshot = std::map<int, ThreadCpu>;

inline ThreadCpuSnapshot
thread_cpu_snapshot()
{
  ThreadCpuSnapshot out;
  DIR *dir = opendir("/proc/self/task");
  if (!dir)
    return out;
  while (const dirent *entry = readdir(dir)) {
    const std::string_view tid_str = entry->d_name;
    int tid = 0;
    if (std::from_chars(tid_str.data(), tid_str.data() + tid_str.size(), tid)
            .ec != std::errc{})
      continue;
    std::ifstream stat(std::format("/proc/self/task/{}/stat", tid_str));
    std::string line;
    if (!std::getline(stat, line))
      continue;
    // "tid (name) state ...": the name may hold spaces and parentheses, so
    // it runs to the last ')'.  utime and stime are fields 14 and 15.
    const auto open = line.find('(');
    const auto close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos ||
        close + 2 > line.size())
      continue;
    ThreadCpu cpu{.name = line.substr(open + 1, close - open - 1)};
    std::istringstream rest(line.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 15 && rest >> field; i++)
      if (i >= 14) {
        uint64_t ticks = 0;
        std::from_chars(field.data(), field.data() + field.size(), ticks);
        cpu.ticks += ticks;
      }
    out.emplace(tid, std::move(cpu));
  }
  closedir(dir);
  return out;
}

// The cores this process used between two snapshots wall_s seconds apart,
// and its busiest threads as "name:tid" with the cores each used.
struct ThreadCpuUse {
  double cores = 0;
  std::vector<std::pair<double, std::string>> busiest;
};

inline ThreadCpuUse
thread_cpu_use(const ThreadCpuSnapshot &before, const ThreadCpuSnapshot &after,
               double wall_s, size_t nbusiest = 4)
{
  ThreadCpuUse use;
  const auto hz = static_cast<double>(sysconf(_SC_CLK_TCK));
  if (hz <= 0 || wall_s <= 0)
    return use;
  for (const auto &[tid, cpu] : after) {
    const auto it = before.find(tid);
    const uint64_t start = it == before.end() ? 0 : it->second.ticks;
    const double cores = static_cast<double>(cpu.ticks - start) / hz / wall_s;
    use.cores += cores;
    use.busiest.emplace_back(cores, std::format("{}:{}", cpu.name, tid));
  }
  std::ranges::sort(use.busiest, std::greater{});
  if (use.busiest.size() > nbusiest)
    use.busiest.resize(nbusiest);
  return use;
}

} // namespace hogd
