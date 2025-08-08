
#include <array>
#include <filesystem>
#include <print>
#include <regex>
#include <thread>
#include <unordered_map>

#include "x23/cpumap.h"
#include "x23/fileio.h"

namespace x23 {

constinit CpuMap core_map(nullptr);
constinit CpuMap llc_map(nullptr);

void
init_cpumaps()
{
  static std::atomic_int state = 0;
  if (state < 2) {
    if (int zero = 0; state.compare_exchange_strong(zero, 1)) {
      core_map = make_cpu_map("topology/core_id");
      llc_map = make_llc_map();
      state = 2;
      state.notify_all();
    }
    else
      state.wait(1);
  }
}
static const bool _init = (init_cpumaps(), false);

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
