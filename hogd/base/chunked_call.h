#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/chunk_items.h"
#include "hogd/base/chunk_position.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "x23/async.h"

namespace hogd {

namespace chunked_call_detail {

// What one run's workers share. It lives in call_in_chunks's frame, which
// outlives every worker.
template<typename T, typename Call> struct ChunkRun {
  std::vector<std::vector<T>> parts;
  size_t max_items_per_chunk;
  size_t chunk_count;
  Call call;
  std::atomic<size_t> next_chunk{0};
  std::atomic<bool> failed{false};
};

// The pieces of [parts] that hold the items at positions [begin, begin +
// count), numbered across the parts in order; fewer if the parts end first.
template<typename T>
std::vector<std::span<const T>>
slice_parts(const std::vector<std::vector<T>> &parts, size_t begin,
            size_t count)
{
  std::vector<std::span<const T>> pieces;
  size_t part_begin = 0;
  for (const std::vector<T> &part : parts) {
    if (part_begin >= begin + count) {
      break;
    }
    const size_t part_end = part_begin + part.size();
    if (part_end > begin) {
      const size_t from = std::max(begin, part_begin) - part_begin;
      const size_t to = std::min(begin + count, part_end) - part_begin;
      pieces.push_back(std::span<const T>(part).subspan(from, to - from));
    }
    part_begin = part_end;
  }
  return pieces;
}

// One worker of a run: on the thread pool, takes the next unsent chunk,
// builds it, and awaits its call, until the run is exhausted or a call has
// failed. A call's exception is the worker's result.
template<typename T, typename Call>
x23::Async<void>
send_chunks(ChunkRun<T, Call> *run)
{
  co_await default_coropoll().pool_.bg();
  for (size_t index = run->next_chunk.fetch_add(1, std::memory_order_relaxed);
       index < run->chunk_count && !run->failed.load(std::memory_order_relaxed);
       index = run->next_chunk.fetch_add(1, std::memory_order_relaxed)) {
    const std::vector<std::span<const T>> pieces =
        slice_parts(run->parts, index * run->max_items_per_chunk,
                    run->max_items_per_chunk);
    const ChunkPosition position{.index = static_cast<std::uint32_t>(index),
                                 .count = static_cast<std::uint32_t>(
                                     run->chunk_count)};
    try {
      co_await run->call(position, ChunkItems<T>(pieces));
    } catch (...) {
      run->failed.store(true, std::memory_order_relaxed);
      throw;
    }
  }
}

} // namespace chunked_call_detail

// Sends the items of [parts] (one vector per producer, such as per thread)
// as a run of chunks of at most [max_items_per_chunk] items, numbered across
// the parts in order. Each chunk is sent by its own coroutine on the thread
// pool, with at most [max_in_flight] chunks in flight, so XDR serialization
// runs in parallel and a multiclient spreads the calls over its connections.
// [call](ChunkPosition, ChunkItems<T>) returns the x23::Async<void> that
// issues one chunk's call; it runs on pool threads, several at a time, and
// its items are spans into [parts], valid until that Async resolves, so a
// call serializes them in place and nothing copies an item. The run resolves
// once every call has replied, or fails with a call's exception once the
// other calls in flight have settled, with no further chunk issued. A run
// always has at least one chunk, so an empty input still makes one call with
// no items.
template<typename T, typename Call>
x23::Async<void>
call_in_chunks(std::vector<std::vector<T>> parts, size_t max_items_per_chunk,
               Call call, size_t max_in_flight = available_parallelism())
{
  CHECK_GT(max_items_per_chunk, 0UL);
  CHECK_GT(max_in_flight, 0UL);
  size_t total = 0;
  for (const std::vector<T> &part : parts) {
    total += part.size();
  }
  const size_t chunk_count =
      std::max<size_t>(1,
                       (total + max_items_per_chunk - 1) / max_items_per_chunk);
  CHECK_LE(chunk_count, std::numeric_limits<std::uint32_t>::max());

  chunked_call_detail::ChunkRun<T, Call> run{.parts = std::move(parts),
                                             .max_items_per_chunk =
                                                 max_items_per_chunk,
                                             .chunk_count = chunk_count,
                                             .call = std::move(call)};
  const size_t worker_count = std::min(chunk_count, max_in_flight);
  std::vector<x23::Async<void>> workers;
  workers.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    workers.push_back(chunked_call_detail::send_chunks(&run));
  }
  std::exception_ptr failure;
  for (x23::Async<void> &worker : workers) {
    try {
      co_await worker;
    } catch (...) {
      if (!failure) {
        failure = std::current_exception();
      }
    }
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
}

} // namespace hogd
