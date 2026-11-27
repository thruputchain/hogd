// libc_shims.cc - PointerTagSanitizer shims for glibc
//
// See shim.h. PTSan's runtime wraps the libc functions it knows about. A call
// to any other one passes tagged pointers straight into glibc, and memory
// glibc allocates during it comes back tagged. These are the functions that
// PTSan 0.4.0 does not wrap and that code in a hogd binary calls with a
// pointer: hogd itself, x23, RocksDB, GoogleTest and the PTSan libc++. To
// look for more, list what a PTSan-built binary still imports from libc:
//
//   llvm-nm -D --undefined-only build/ptsan/hogd/base/base_test
//
// Functions the runtime wraps are defined in the binary and so are absent
// from that list. dlsym() is on it and has no shim, because the shims find
// the next definition with it. Its one caller in instrumented code is
// RocksDB's dynamic library loader, which hogd does not use.

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <new>

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
HOGD_PTSAN_INTERPOSED(fchmodat);
HOGD_PTSAN_INTERPOSED(fcntl);
HOGD_PTSAN_INTERPOSED(fdopendir);
HOGD_PTSAN_INTERPOSED(fread_unlocked);
HOGD_PTSAN_INTERPOSED(madvise);
HOGD_PTSAN_INTERPOSED(msync);
HOGD_PTSAN_INTERPOSED(posix_madvise);
HOGD_PTSAN_INTERPOSED(sched_setscheduler);
HOGD_PTSAN_INTERPOSED(symlink);
HOGD_PTSAN_INTERPOSED(wcscasecmp);

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

// std::filesystem::permissions() calls this.
int
fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(fchmodat)(dirfd, raw(pathname), mode, flags);
}

// The optional argument is a pointer for the lock, owner and write-hint
// commands and an integer otherwise. RocksDB locks its database directory
// with F_SETLK.
int
fcntl(int fd, int cmd, ...)
{
  va_list args;
  va_start(args, cmd);
  unsigned long arg = va_arg(args, unsigned long);
  va_end(args);
  switch (cmd) {
  case F_GETLK:
  case F_SETLK:
  case F_SETLKW:
  case F_OFD_GETLK:
  case F_OFD_SETLK:
  case F_OFD_SETLKW:
  case F_GETOWN_EX:
  case F_SETOWN_EX:
  case F_GET_RW_HINT:
  case F_SET_RW_HINT:
  case F_GET_FILE_RW_HINT:
  case F_SET_FILE_RW_HINT:
    arg = ptsan_get_address(reinterpret_cast<const void *>(arg));
    break;
  default:
    break;
  }
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(fcntl)(fd, cmd, arg);
}

// Allocates the DIR it returns. std::filesystem::remove_all() calls this too.
DIR *
fdopendir(int fd)
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(fdopendir)(fd);
}

// RocksDB reads its sequential files with this.
size_t
fread_unlocked(void *ptr, size_t size, size_t n, FILE *stream)
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(fread_unlocked)(raw(ptr), size, n, raw(stream));
}

int
madvise(void *addr, size_t length, int advice) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(madvise)(raw(addr), length, advice);
}

// RocksDB syncs files it writes through mmap with this.
int
msync(void *addr, size_t length, int flags)
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(msync)(raw(addr), length, flags);
}

// RocksDB passes access hints for files it reads through mmap with this.
int
posix_madvise(void *addr, size_t length, int advice) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(posix_madvise)(raw(addr), length, advice);
}

// RocksDB lowers the priority of its background threads with this.
int
sched_setscheduler(pid_t pid, int policy, const sched_param *param) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(sched_setscheduler)(pid, policy, raw(param));
}

// std::filesystem::create_symlink() calls this.
int
symlink(const char *target, const char *linkpath) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(symlink)(raw(target), raw(linkpath));
}

// GoogleTest compares wide strings with this for EXPECT_STRCASEEQ.
int
wcscasecmp(const wchar_t *s1, const wchar_t *s2) noexcept
{
  UninstrumentedCall call;
  return HOGD_PTSAN_NEXT(wcscasecmp)(raw(s1), raw(s2));
}

} // extern "C"

// Over-aligned operator new is here because of one more function PTSan does
// not wrap, aligned_alloc(). The C++ runtime implements these operators with
// it, glibc serves it, and its memory has no object ID: PTSan would check no
// access to a heap object of an alignas(64) type. PTSan does wrap
// posix_memalign(), which wants an alignment that is a multiple of the
// pointer size.
//
// aligned_alloc() itself cannot be redirected the same way. The runtime also
// allocates exceptions with it, and the system unwinder, which is not
// instrumented, reads those.
//
// The definitions are weak so that a test which replaces operator new, as
// hogd/base/test_support/alloc_fault.cc does, still links. The runtime's
// operator delete releases the memory with free().

namespace {

// Returns nullptr once allocation fails and no new-handler is installed.
void *
aligned_new(std::size_t size, std::align_val_t alignment)
{
  std::size_t align =
      std::max(static_cast<std::size_t>(alignment), sizeof(void *));
  for (;;) {
    void *ptr = nullptr;
    if (posix_memalign(&ptr, align, size == 0 ? 1 : size) == 0) {
      return ptr;
    }
    std::new_handler handler = std::get_new_handler();
    if (handler == nullptr) {
      return nullptr;
    }
    handler();
  }
}

} // namespace

[[gnu::weak]] void *
operator new(std::size_t size, std::align_val_t alignment)
{
  if (void *ptr = aligned_new(size, alignment)) {
    return ptr;
  }
  throw std::bad_alloc();
}

[[gnu::weak]] void *
operator new(std::size_t size, std::align_val_t alignment,
             const std::nothrow_t &) noexcept
{
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

[[gnu::weak]] void *
operator new[](std::size_t size, std::align_val_t alignment)
{
  return ::operator new(size, alignment);
}

[[gnu::weak]] void *
operator new[](std::size_t size, std::align_val_t alignment,
               const std::nothrow_t &) noexcept
{
  try {
    return ::operator new[](size, alignment);
  } catch (...) {
    return nullptr;
  }
}
