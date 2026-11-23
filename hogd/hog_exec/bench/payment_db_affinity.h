#pragma once

#ifdef __linux__
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hogd::payment_db_affinity {

inline std::string
read(const std::filesystem::path &path)
{
  std::ifstream file(path);
  std::string line;
  if (!std::getline(file, line) || line.empty())
    throw std::runtime_error("failed to read " + path.string());
  return line;
}

inline cpu_set_t
current_mask(pid_t tid = 0)
{
  cpu_set_t mask;
  if (sched_getaffinity(tid, sizeof(mask), &mask) != 0)
    throw std::runtime_error("sched_getaffinity failed");
  return mask;
}

inline cpu_set_t
parse_mask(std::string_view list)
{
  cpu_set_t mask;
  CPU_ZERO(&mask);
  while (!list.empty()) {
    auto comma = list.find(',');
    auto item = list.substr(0, comma);
    auto dash = item.find('-');
    int lo = std::stoi(std::string(item.substr(0, dash)));
    int hi =
        dash == item.npos ? lo : std::stoi(std::string(item.substr(dash + 1)));
    if (lo < 0 || hi < lo || hi >= CPU_SETSIZE)
      throw std::runtime_error("invalid CPU list");
    for (int cpu = lo; cpu <= hi; ++cpu)
      CPU_SET(cpu, &mask);
    if (comma == list.npos)
      break;
    list.remove_prefix(comma + 1);
  }
  return mask;
}

inline cpu_set_t
select(std::string_view arm, const cpu_set_t &allowed,
       const std::filesystem::path &root = "/sys/devices/system/cpu")
{
  if (arm == "stock")
    return allowed;

  // Keep selected cache domains within one NUMA node. Entries under cpuN/nodeN
  // identify the node even when SMT siblings have disjoint CPU numbers.
  std::map<std::string, std::map<std::string, std::vector<int>>> topology;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &allowed))
      continue;
    const auto path = root / ("cpu" + std::to_string(cpu));
    const auto siblings =
        parse_mask(read(path / "topology/thread_siblings_list"));
    bool lowest = true;
    for (int lower = 0; lower < cpu; ++lower) {
      if (CPU_ISSET(lower, &siblings) && CPU_ISSET(lower, &allowed))
        lowest = false;
    }
    if (!lowest)
      continue;

    std::string node, llc;
    int level = 0;
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
      auto name = entry.path().filename().string();
      if (name.starts_with("node") && name.size() > 4 && name[4] >= '0' &&
          name[4] <= '9')
        node = name;
    }
    for (const auto &cache :
         std::filesystem::directory_iterator(path / "cache")) {
      if (!cache.path().filename().string().starts_with("index") ||
          read(cache.path() / "type") != "Unified")
        continue;
      const int candidate = std::stoi(read(cache.path() / "level"));
      if (candidate > level) {
        level = candidate;
        llc = read(cache.path() / "shared_cpu_list");
      }
    }
    if (node.empty() || level < 3)
      throw std::runtime_error("CPU lacks auditable NUMA/LLC topology");
    const auto cache_mask = parse_mask(llc);
    if (!CPU_ISSET(cpu, &cache_mask))
      throw std::runtime_error("CPU lacks auditable NUMA/LLC topology");
    topology[node][llc].push_back(cpu);
  }

  constexpr size_t width = 8;
  for (const auto &[node, domains] : topology) {
    std::vector<const std::vector<int> *> eligible;
    for (const auto &[llc, cores] : domains) {
      if (cores.size() >= width)
        eligible.push_back(&cores);
    }
    if (eligible.size() < 2)
      continue;

    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (size_t i = 0; i < width; ++i) {
      const size_t domain = arm == "llc-a-1" ? 0 : arm == "llc-b-1" ? 1 : i / 4;
      const size_t index = arm.starts_with("split-") ? i % 4 : i;
      CPU_SET((*eligible[domain])[index], &mask);
    }
    return mask;
  }
  throw std::runtime_error(
      "need two LLCs on one NUMA node with eight allowed physical cores each");
}

} // namespace hogd::payment_db_affinity
#endif
