#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"

#include <x23/cpumap.h>
#include <x23/debug.h>
#include <x23/event.h>
#include <x23/threadpool.h>

namespace hogd {

inline x23::ThreadPool &
default_threadpool()
{
  static x23::ThreadPool pool{available_parallelism()};
  return pool;
}

inline x23::CoroPoll &
default_coropoll()
{
  static x23::CoroPoll cp{default_threadpool()};
  return cp;
}

// Inclusive-exclusive half-open range used when slicing transaction batches.
struct ChunkRange {
  size_t start;
  size_t end; // [start, end)
};

inline std::vector<ChunkRange>
make_chunks(size_t total_size, size_t chunk_count)
{
  std::vector<ChunkRange> chunks;
  if (!total_size || !chunk_count) {
    return chunks;
  }
  chunk_count = std::min(chunk_count, total_size);

  const size_t base_size = total_size / chunk_count;
  const size_t remainder = total_size % chunk_count;

  size_t offset = 0;
  chunks.reserve(chunk_count);
  for (size_t i = 0; i < chunk_count; ++i) {
    // Distribute remainder across the first [remainder] chunks to keep them as
    // balanced as possible.
    const size_t chunk_size = base_size + (i < remainder ? 1 : 0);
    const size_t start = offset;
    const size_t end = start + chunk_size;
    chunks.push_back(ChunkRange{.start = start, .end = end});
    offset = end;
  }
  CHECK_EQ(offset, total_size);

  return chunks;
}

inline std::vector<ChunkRange>
make_chunks_for_max_size(size_t total_size, size_t max_chunk_size)
{
  if (!max_chunk_size) {
    return {};
  }
  // Round up so we never exceed [max_chunk_size] in any bucket.
  const size_t chunk_count = (total_size + max_chunk_size - 1) / max_chunk_size;
  return make_chunks(total_size, chunk_count);
}

} // namespace hogd
