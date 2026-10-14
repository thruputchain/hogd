#include "x23/cpumap.h"

#include <algorithm> // IWYU pragma: keep
#include <array>
#include <bit>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map> // IWYU pragma: keep
#include <utility>

#include "x23/config.h"
#include "x23/debug.h" // IWYU pragma: keep
#include "x23/fileio.h"
#include "x23/nodestroy.h"
#include "x23/vec.h"

namespace x23 {

#if HAVE_SCHED_SETAFFINITY
Unconstructed<const CpuSet> allowed_cpus;
#endif // HAVE_SCHED_SETAFFINITY
Unconstructed<const CpuMap> core_map;
Unconstructed<const CpuMap> llc_map;
Unconstructed<const CpuMap> single_bucket_map;

std::tuple<>
init_cpumaps()
{
  LsanDisabler _inhibit;
  static const auto placeholder = [] {
#if HAVE_SCHED_SETAFFINITY
    allowed_cpus.construct(get_allowed_cpus());
#endif // HAVE_SCHED_SETAFFINITY
    core_map.construct(make_cpu_map("topology/core_id"));
    llc_map.construct(make_llc_map());
    single_bucket_map.construct(make_single_bucket_map());
    return std::tuple{};
  }();
  return placeholder;
}
static const auto _init = init_cpumaps();

std::string
get_field(std::string_view field, std::filesystem::path file)
{
  auto fieldsize = field.size();
  auto contents = readfile<std::string>(std::move(file));
  if (!contents)
    return {};
  for (std::string_view tail{*contents}; !tail.empty();) {
    auto pos = tail.find('\n');
    std::string_view head = tail.substr(0, pos);

    if (head.size() > fieldsize && head.starts_with(field) &&
        head[fieldsize] == ':') {
      auto i = fieldsize + 1;
      while (i < head.size() && std::isspace(head[i]))
        ++i;
      return std::string{head.substr(i)};
    }

    if (pos == std::string_view::npos)
      break;
    tail.remove_prefix(pos + 1);
  }
  return {};
}

#if HAVE_SCHED_SETAFFINITY
Errno
CpuSet::set_affinity(pid_t pid) const
{
  if (sched_setaffinity(pid, size(), set()) == 0)
    return 0;
  Errno err;
  debug.err("sched_setaffinity({}): {}", *this, err);
  return err;
}

CpuSet
parse_cpuset(std::string_view list)
{
  const char *p = list.data();
  auto lex = [&p, e = p + list.size()] -> std::optional<long> {
    while (p < e && (*p == ',' || isspace(*p)))
      ++p;
    if (p < e) {
      char *nextp;
      auto ret = strtol(p, &nextp, 10);
      if (p != nextp) {
        p = nextp;
        return ret;
      }
    }
    return std::nullopt;
  };

  long num_cpu = 0;
  std::vector<std::pair<unsigned, unsigned>> elems;
  while (auto token = lex()) {
    unsigned int val =
        std::min<unsigned>(std::labs(*token), CpuSet::kNCPUs - 1);
    if (val >= num_cpu)
      num_cpu = val + 1;
    if (*token >= 0)
      elems.emplace_back(val, val);
    else if (elems.empty())
      return CpuSet{};
    else
      elems.back().second = std::max(elems.back().first, val);
  }

  CpuSet ret;
  for (auto [i, last] : elems)
    for (;; ++i) {
      ret.set(i);
      if (i == last)
        break;
    }
  return ret;
}

void
CpuMap::compute_cpusets()
{
  cpusets_.resize(nbuckets_);
  for (size_t i = 0; i < map_.size(); ++i)
    cpusets_[bucket(i)].set(i);
}
#endif // HAVE_SCHED_SETAFFINITY

unsigned
nproc()
{
  // NOLINTNEXTLINE(readability-identifier-naming)
  constexpr std::array<int, 256> hex_nibble_popcount = []() consteval {
    std::array<int, 256> counts{};
    for (unsigned c = '0'; c <= '9'; ++c)
      counts[c] = std::popcount(c - '0');
    for (unsigned c = 'A'; c <= 'F'; ++c)
      counts[c] = counts[c | 0x20] = std::popcount(c - 'A' + 10);
    return counts;
  }();

  auto allowed = get_field("Cpus_allowed", "/proc/self/status");
  unsigned sum = 0;
  for (char c : allowed)
    sum += hex_nibble_popcount[c];
  if (sum)
    return sum;
  return std::thread::hardware_concurrency();
}

#if HAVE_SCHED_GETCPU
CpuMap
make_cpu_map(std::string_view path)
try {
  // The buckets are unique numbers but not always contiguous, so map
  // them to a contiguous range of integers in [0,N).
  int nextbucket = 0;
  std::unordered_map<std::string, int> bucketmap;
  auto bucket = [&](std::string b) {
    if (auto [it, ok] = bucketmap.try_emplace(std::move(b), nextbucket); !ok)
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
      debug("{}: {}", idpath.string(), idstr.error());
      // This probably means that we are on a mobile CPU with some
      // "efficiency cores" missing an L3 cache.  That's not the kind
      // of high-core-count or multi-socket machine where L3-cache
      // affinity really helps, so just return a single bucket.
      return CpuMap{Vec{0}};
    }
    ret[cpu] = bucket(std::move(*idstr));
  }
  return CpuMap{ret};
} catch (...) {
  return CpuMap{Vec{0}};
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
  return CpuMap{Vec{0}};
} catch (...) {
  return CpuMap{Vec{0}};
}

CpuMap
make_single_bucket_map()
{
  return CpuMap{Vec{0}};
}

#else  // !HAVE_SCHED_GETCPU
CpuMap
make_cpu_map(std::string_view /*path*/)
{
  return CpuMap{Vec{0}};
}
CpuMap
make_llc_map()
{
  return CpuMap{Vec{0}};
}
CpuMap
make_single_bucket_map()
{
  return CpuMap{Vec{0}};
}
#endif // !HAVE_SCHED_GETCPU

} // namespace x23
