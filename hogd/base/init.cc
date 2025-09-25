// init.cc - initialization for the base library

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

GStatus<void>
base_init()
{
  struct utsname buf;
  int cret = uname(&buf);
  if (cret < 0)
    return GError(GCode::kInternal);

  info("pid:{}, os:{}, arch:{}, cores:{}", getpid(), auto(buf.sysname),
       auto(buf.machine), available_parallelism());
  return {};
}

} // namespace hogd
