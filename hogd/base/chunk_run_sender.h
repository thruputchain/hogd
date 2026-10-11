#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <exception>
#include <limits>
#include <ranges>
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

// A range of parts, each a contiguous container of T, such as a std::vector
// or an x23::Vec per producing thread.
template<typename Parts, typename T>
concept parts_of =
    std::ranges::forward_range<Parts> &&
    std::same_as<std::ranges::range_value_t<std::ranges::range_value_t<Parts>>,
                 T> &&
    requires(const std::ranges::range_value_t<Parts> &part) {
      { std::ranges::data(part) } -> std::convertible_to<const T *>;
      { std::ranges::size(part) } -> std::convertible_to<size_t>;
    };

// Sends a message too large for one RPC record as a run of chunks (see
// ChunkPosition). The items come as one contiguous container per producer,
// such as per thread, and are numbered across those parts in order into
// chunks of at most [max_items_per_chunk] items; a run always has at least one
// chunk, so an empty message makes one call with no items. The sender views
// the parts rather than owning them, so they must outlive the send and one
// message's parts can feed a sender per subscriber. [call](ChunkPosition,
// ChunkItems<T>) returns the x23::Async<void> that issues one chunk's call. It
// runs on pool threads, several at a time, and its items are spans into the
// parts, valid until that Async resolves, so a call serializes them in place.
template<typename T, typename Call> class ChunkRunSender {
public:
  template<parts_of<T> Parts>
  ChunkRunSender(const Parts &parts, size_t max_items_per_chunk, Call call)
    : max_items_per_chunk_(max_items_per_chunk), call_(std::move(call))
  {
    CHECK_GT(max_items_per_chunk, 0UL);
    size_t total = 0;
    for (const auto &part : parts) {
      parts_.emplace_back(std::ranges::data(part), std::ranges::size(part));
      total += parts_.back().size();
    }
    chunk_count_ = std::max<size_t>(1, (total + max_items_per_chunk - 1) /
                                           max_items_per_chunk);
    CHECK_LE(chunk_count_, std::numeric_limits<std::uint32_t>::max());
  }

  // A temporary owning container would leave the spans dangling once the
  // statement ends; a view such as a std::span may still be passed by value.
  template<parts_of<T> Parts> requires (!std::ranges::borrowed_range<Parts>)
  ChunkRunSender(const Parts &&parts, size_t max_items_per_chunk,
                 Call call) = delete;

  // Issues every chunk's call from coroutines on the thread pool, at most
  // [max_in_flight] at once. Resolves once every call has replied, or fails
  // with a call's exception once the other calls in flight have settled. A
  // failure stops the workers from taking further chunks; a chunk a worker had
  // already started still goes out. A run is sent once, and the sender must
  // outlive the returned Async.
  x23::Async<void> send(size_t max_in_flight = available_parallelism())
  {
    CHECK_GT(max_in_flight, 0UL);
    CHECK_EQ(next_chunk_.load(std::memory_order_relaxed), 0UL);
    const size_t worker_count = std::min(chunk_count_, max_in_flight);
    std::vector<x23::Async<void>> workers;
    workers.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
      workers.push_back(worker());
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

private:
  // One worker of send(): takes the next unsent chunk, builds it on the thread
  // pool, and awaits its call, until the run is exhausted or a call has
  // failed. A reply resumes this coroutine on the thread that read it, a
  // connection's reader, so it hops back to the pool before every chunk:
  // serializing there would stall every other reply on that connection. A
  // call's exception is the worker's result.
  x23::Async<void> worker()
  {
    for (size_t index = next_chunk_.fetch_add(1, std::memory_order_relaxed);
         index < chunk_count_ && !failed_.load(std::memory_order_relaxed);
         index = next_chunk_.fetch_add(1, std::memory_order_relaxed)) {
      co_await default_coropoll().pool_.bg();
      const std::vector<std::span<const T>> pieces = pieces_of(index);
      const ChunkPosition position{.index = static_cast<std::uint32_t>(index),
                                   .count = static_cast<std::uint32_t>(
                                       chunk_count_)};
      try {
        co_await call_(position, ChunkItems<T>(pieces));
      } catch (...) {
        failed_.store(true, std::memory_order_relaxed);
        throw;
      }
    }
  }

  // The pieces of the parts that hold chunk [index]'s items: the items at
  // positions [index * max_items_per_chunk, (index + 1) * max_items_per_chunk)
  // numbered across the parts, fewer if the parts end first.
  std::vector<std::span<const T>> pieces_of(size_t index) const
  {
    const size_t begin = index * max_items_per_chunk_;
    const size_t end = begin + max_items_per_chunk_;
    std::vector<std::span<const T>> pieces;
    size_t part_begin = 0;
    for (const std::span<const T> &part : parts_) {
      if (part_begin >= end) {
        break;
      }
      const size_t part_end = part_begin + part.size();
      if (part_end > begin) {
        const size_t from = std::max(begin, part_begin) - part_begin;
        const size_t to = std::min(end, part_end) - part_begin;
        pieces.push_back(part.subspan(from, to - from));
      }
      part_begin = part_end;
    }
    return pieces;
  }

  std::vector<std::span<const T>> parts_;
  size_t max_items_per_chunk_;
  size_t chunk_count_ = 0;
  Call call_;
  std::atomic<size_t> next_chunk_{0};
  std::atomic<bool> failed_{false};
};

template<typename Parts, typename Call>
ChunkRunSender(const Parts &, size_t, Call) -> ChunkRunSender<
    std::ranges::range_value_t<std::ranges::range_value_t<Parts>>, Call>;

} // namespace hogd
