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

  #ifdef __APPLE__
  // Logging library is broken on mac with clang
  info("pid:{}, os:{}, arch:{}, cores:{}", getpid(), std::string(buf.sysname), std::string(buf.machine),
       available_parallelism());
  #else
  info("pid:{}, os:{}, arch:{}, cores:{}", getpid(), buf.sysname, buf.machine,
       available_parallelism());
  #endif
  return {};
}

} // namespace hogd
