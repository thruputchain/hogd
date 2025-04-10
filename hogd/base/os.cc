#include "hogd/base/os.h"

extern "C" {
#if defined(__linux__)
#include <sched.h>
#endif // defined(__linux__)
}

#include <cerrno>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"

namespace hogd {

size_t
available_parallelism()
{
#if defined(__linux__)
  // sched_getaffinity() determines the number of cores the program is allowed
  // to run on based on taskset instead of the number of cores in the machine.
  cpu_set_t mask;
  if (sched_getaffinity(0, sizeof(mask), &mask) == 0)
    return CPU_COUNT(&mask);
#endif // defined(__linux__)

  // Not as accurate; only reports the number of cores in the machine.
  return std::thread::hardware_concurrency();
}

ErrnoStatus<ProcessStats>
read_process_stats()
{
#if defined(__linux__)
  ErrnoStatus<std::string> stat = read_sys_file("/proc/self/stat");
  if (!stat)
    return stat.error();

  std::vector<std::string_view> values = split(*stat);
  auto parse = [&values](int idx) {
    return parse_integer<size_t>(values.at(idx)).value();
  };
  try {
    return ProcessStats{
        .utime = parse(13),
        .stime = parse(14),
        .iotime = parse(41),
        .vsize = parse(22),
        .rss = parse(23),
        .nthreads = parse(19),
    };
  } catch (const std::exception &e) {
    return Errno(EINVAL);
  }
#else  // defined(__linux__)
  return Errno(ENOENT);
#endif // defined(__linux__)
}

} // namespace hogd
