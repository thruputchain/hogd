// libc_shims.cc - PointerTagSanitizer shims for glibc
//
// See shim.h. PTSan's runtime wraps the libc functions it knows about. A call
// to any other one passes tagged pointers straight into glibc, and memory
// glibc allocates during it comes back tagged. These are the functions hogd,
// x23 and the PTSan libc++ use that PTSan 0.4.0 does not wrap. To look for
// more, list what a PTSan-built binary still imports from libc:
//
//   llvm-nm -D --undefined-only build/ptsan/hogd/base/base_test
//
// Functions the runtime wraps are defined in the binary and so are absent
// from that list.

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>

#include "hogd/ptsan/shim.h"

using hogd::ptsan::raw;
using hogd::ptsan::UninstrumentedCall;

extern "C" {
// glibc exports this for the C++ runtime and declares it in no header.
int __cxa_thread_atexit_impl(void (*func)(void *), void *obj,
                             void *dso_symbol) noexcept;
}

HOGD_PTSAN_INTERPOSED(__cxa_thread_atexit_impl);
HOGD_PTSAN_INTERPOSED(__sched_cpucount);
HOGD_PTSAN_INTERPOSED(faccessat);
HOGD_PTSAN_INTERPOSED(fdopendir);
HOGD_PTSAN_INTERPOSED(fchmodat);
HOGD_PTSAN_INTERPOSED(symlink);

extern "C" {

// Registers the destructor of a thread_local object, in a list node it
// allocates. obj stays tagged: glibc only hands it back to func.
int
__cxa_thread_atexit_impl(void (*func)(void *), void *obj,
                         void *dso_symbol) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(__cxa_thread_atexit_impl)(func, obj, raw(dso_symbol));
}

// CPU_COUNT() and CPU_COUNT_S() expand to this.
int
__sched_cpucount(size_t setsize, const cpu_set_t *setp) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(__sched_cpucount)(setsize, raw(setp));
}

int
faccessat(int dirfd, const char *pathname, int mode, int flags) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(faccessat)(dirfd, raw(pathname), mode, flags);
}

// Allocates the DIR it returns. std::filesystem::remove_all() calls this too.
DIR *
fdopendir(int fd)
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(fdopendir)(fd);
}

// std::filesystem::permissions() calls this.
int
fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(fchmodat)(dirfd, raw(pathname), mode, flags);
}

// std::filesystem::create_symlink() calls this.
int
symlink(const char *target, const char *linkpath) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(symlink)(raw(target), raw(linkpath));
}

} // extern "C"
