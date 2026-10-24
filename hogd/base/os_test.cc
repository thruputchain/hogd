#include <gtest/gtest.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>

#include "hogd/base/os.h"
#include "hogd/base/types.h"

namespace hogd {
namespace {

// Child exit codes for ResidentMemoryBytesTracksTouchedPages.
constexpr int kRssOk = 0;
constexpr int kRssMmapFailed = 2;
constexpr int kRssNotReported = 3;
constexpr int kRssOutOfRange = 4;

// ThreadSanitizer charges its shadow memory to the process's RSS. Every
// instrumented store writes a shadow cell, and shadow addresses are spaced
// four times further apart than the bytes they describe, so the single byte
// the child writes per page dirties a shadow page of its own: RSS grows by
// about twice the region touched, not once. AddressSanitizer does not do
// this -- its shadow is written only when memory is poisoned, which a plain
// anonymous mapping never is -- so it keeps the tight bound.
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define HOGD_SANITIZER_SHADOW_IN_RSS 1
#endif
#endif
#ifdef __SANITIZE_THREAD__
#define HOGD_SANITIZER_SHADOW_IN_RSS 1
#endif
#ifndef HOGD_SANITIZER_SHADOW_IN_RSS
#define HOGD_SANITIZER_SHADOW_IN_RSS 0
#endif

// Upper bound on the child's RSS growth, as a percentage of the region it
// touched. The shadow doubles the growth, so the sanitized bound leaves room
// for that and a little more; it is still many orders of magnitude below the
// virtual size (tens of terabytes under TSAN), which is what this bound is
// really guarding against resident_memory_bytes() reporting.
#if HOGD_SANITIZER_SHADOW_IN_RSS
constexpr size_t kMaxGrowthPercent = 300;
#else
constexpr size_t kMaxGrowthPercent = 125;
#endif

TEST(OsTest, ResidentMemoryBytesTracksTouchedPages)
{
  constexpr size_t kRegionBytes = size_t{128} << 20;
  // Measure in a fresh child process so nothing else in this test binary can
  // move its RSS between the two reads.
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    const size_t before = resident_memory_bytes();
    void *region = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
      _exit(kRssMmapFailed);
    }
    // Write one byte per page to force every page to be allocated.
    const auto page_bytes = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto *bytes = static_cast<volatile unsigned char *>(region);
    for (size_t offset = 0; offset < kRegionBytes; offset += page_bytes) {
      bytes[offset] = 1;
    }
    const size_t after = resident_memory_bytes();
    if (before == 0 || after < before) {
      _exit(kRssNotReported);
    }
    const size_t grown = after - before;
    (void)std::fprintf(stderr, "rss before=%zu after=%zu grown=%zu\n", before,
                       after, grown);
    // At least 75% of the region -- allocator and page-table noise is far
    // smaller -- and no more than kMaxGrowthPercent of it.
    const bool near_region = grown >= kRegionBytes * 3 / 4 &&
                             grown <= kRegionBytes * kMaxGrowthPercent / 100;
    _exit(near_region ? kRssOk : kRssOutOfRange);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  // include-cleaner does not credit <sys/wait.h> for the wait macros.
  // NOLINTBEGIN(misc-include-cleaner)
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), kRssOk)
      << "2: mmap failed, 3: RSS not reported, 4: growth outside "
      << (kRegionBytes * 3 / 4) << "-"
      << (kRegionBytes * kMaxGrowthPercent / 100) << " bytes";
  // NOLINTEND(misc-include-cleaner)
}

} // namespace
} // namespace hogd
