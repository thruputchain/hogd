// init.cc - initialization for the base library

#include <cerrno>
#include <cstring>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <string>
#endif

#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/status.h"
#include "hogd/init.h"

namespace hogd {

namespace {

// Raise RLIMIT_NOFILE soft limit to the hard limit.  systemd defaults to
// 1024:524288 for services (and SSH-spawned processes inherit that cap),
// which is too low for RocksDB and RPC-heavy services.  Failure is
// non-fatal so restrictive containers can still start.
void
raise_nofile_soft_limit()
{
  struct rlimit lim;
  if (getrlimit(RLIMIT_NOFILE, &lim) != 0) {
    info("getrlimit(RLIMIT_NOFILE) failed: {}", std::strerror(errno));
    return;
  }
  if (lim.rlim_cur >= lim.rlim_max)
    return;

  rlim_t old_soft = lim.rlim_cur;
  lim.rlim_cur = lim.rlim_max;
  if (setrlimit(RLIMIT_NOFILE, &lim) != 0) {
    info("setrlimit(RLIMIT_NOFILE) failed (soft={}, hard={}): {}", old_soft,
         lim.rlim_max, std::strerror(errno));
    return;
  }
  info("raised RLIMIT_NOFILE soft from {} to {}", old_soft, lim.rlim_cur);
}

} // namespace

GStatus<void>
base_init()
{
  struct utsname buf;
  int cret = uname(&buf);
  if (cret < 0)
    return GError(GCode::kInternal);

  raise_nofile_soft_limit();

  info("pid:{}, os:{}, arch:{}, cores:{}", getpid(), auto(buf.sysname),
       auto(buf.machine), available_parallelism());
  return {};
}

} // namespace hogd
