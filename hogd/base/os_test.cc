#include <gtest/gtest.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>

#include "hogd/base/os.h"

namespace hogd {
namespace {

// Child exit codes for ResidentMemoryBytesTracksTouchedPages.
constexpr int kRssOk = 0;
constexpr int kRssMmapFailed = 2;
constexpr int kRssNotReported = 3;
constexpr int kRssOutOfRange = 4;

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
    // Within 25% of the region; allocator and page-table noise is far smaller.
    const bool near_region =
        grown >= kRegionBytes * 3 / 4 && grown <= kRegionBytes * 5 / 4;
    _exit(near_region ? kRssOk : kRssOutOfRange);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), kRssOk)
      << "2: mmap failed, 3: RSS not reported, 4: growth outside 96-160 MiB";
}

} // namespace
} // namespace hogd
