// init.cc - initialization for the base library

extern "C" {
#include <sys/utsname.h>
#if defined(__linux__)
#include <sched.h>
#endif // defined(__linux__)
}

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/init.h"

namespace hogd {

namespace {

#if defined(__linux__)
GStatus<int>
get_available_cores()
{
  // sched_getaffinity() determines the number of cores the program is allowed
  // to run on based on taskset instead of the number of cores in the machine.
  // This is the behavior we want.
  cpu_set_t mask;
  int cret = sched_getaffinity(0, sizeof(mask), &mask);
  if (cret < 0)
    return GError(GCode::kInternal);
  return CPU_COUNT(&mask);
}
#else  // defined(__linux__)
GStatus<int>
get_available_cores()
{
  // TODO(amb): support other targets
  return GError(GCode::kUnsupported);
}
#endif // defined(__linux__)

} // namespace

GStatus<void>
base_init()
{
  struct utsname buf;
  int cret = uname(&buf);
  if (cret < 0)
    return GError(GCode::kInternal);

  GStatus<int> cores = get_available_cores();
  if (!cores) {
    info("os:{}, arch:{}, cores:unknown", buf.sysname, buf.machine);
    return {};
  }

  info("os:{}, arch:{}, cores:{}", buf.sysname, buf.machine, *cores);
  return {};
}

} // namespace hogd
