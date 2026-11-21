#include <benchmark/benchmark.h>

#include <fcntl.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"

namespace hogd {

namespace {

// the number of bytes written per iteration.
constexpr size_t kTotalBytes = 64_MiB;

// BM_BufferedWriterWrite writes kTotalBytes in pieces of the given size through
// a default-size BufferedWriter. The file is on tmpfs and the final flush is
// included so page cache and disk effects do not hide the copy cost.
void
BM_BufferedWriterWrite(benchmark::State &state)
{
  const size_t piece_bytes = state.range(0);
  std::vector<std::byte> piece(piece_bytes, std::byte{0x5a});

  ErrnoStatus<File> file = File::open("/dev/shm", O_TMPFILE | O_RDWR, 0600);
  if (!file) {
    state.SkipWithError("cannot open a temporary file in /dev/shm");
    return;
  }

  for (auto _ : state) {
    // overwrite the same file each iteration to bound tmpfs usage.
    if (!file->lseek(0, SEEK_SET)) {
      state.SkipWithError("lseek failed");
      return;
    }
    BufferedWriter w(*file);
    for (size_t done = 0; done < kTotalBytes; done += piece_bytes) {
      size_t len = std::min(piece_bytes, kTotalBytes - done);
      if (!write_full(w, std::span<const std::byte>(piece.data(), len))) {
        state.SkipWithError("write failed");
        return;
      }
    }
    if (!w.flush()) {
      state.SkipWithError("flush failed");
      return;
    }
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kTotalBytes));
}

BENCHMARK(BM_BufferedWriterWrite)
    ->Arg(64)
    ->Arg(4096)
    ->Arg(46080)
    ->Arg(61440)
    ->UseRealTime();

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0)
    return EXIT_FAILURE;
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
