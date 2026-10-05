#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/base/chunk_items.h"
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_sender.h"
#include "hogd/base/coropoll.h"
#include "x23/async.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {
namespace {

using Items = std::vector<std::uint32_t>;
using Parts = std::vector<Items>;

struct SentChunk {
  ChunkPosition position;
  Items items;
};

// Records every chunk a run sends.
class ChunkRecorder {
public:
  void record(ChunkPosition position, ChunkItems<std::uint32_t> items)
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    Items copied;
    for (const std::span<const std::uint32_t> &piece : items.pieces()) {
      copied.insert(copied.end(), piece.begin(), piece.end());
    }
    sent_.push_back(
        SentChunk{.position = position, .items = std::move(copied)});
  }

  // The chunks sent so far, in index order.
  std::vector<SentChunk> sent()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SentChunk> sent = sent_;
    std::ranges::sort(sent, {}, [](const SentChunk &chunk) {
      return chunk.position.index;
    });
    return sent;
  }

private:
  std::mutex mutex_;
  std::vector<SentChunk> sent_;
};

x23::Async<void>
record(ChunkRecorder *recorder, ChunkPosition position,
       ChunkItems<std::uint32_t> items)
{
  recorder->record(position, items);
  co_return;
}

// A call that records its chunk.
auto
recording(ChunkRecorder *recorder)
{
  return [recorder](ChunkPosition position, ChunkItems<std::uint32_t> items) {
    return record(recorder, position, items);
  };
}

void
expect_position(const SentChunk &chunk, std::uint32_t index,
                std::uint32_t count)
{
  EXPECT_EQ(chunk.position.index, index);
  EXPECT_EQ(chunk.position.count, count);
}

} // namespace

TEST(ChunkRunSender, NumbersItemsAcrossPartsIntoChunksOfAtMostTheLimit)
{
  ChunkRecorder recorder;
  const Parts parts{{0, 1, 2}, {3, 4}, {}, {5, 6}};
  ChunkRunSender sender(parts, /*max_items_per_chunk=*/3, recording(&recorder));
  sender.send()();
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 3U);
  expect_position(sent[0], 0, 3);
  EXPECT_EQ(sent[0].items, (Items{0, 1, 2}));
  expect_position(sent[1], 1, 3);
  EXPECT_EQ(sent[1].items, (Items{3, 4, 5}));
  expect_position(sent[2], 2, 3);
  EXPECT_EQ(sent[2].items, (Items{6}));
}

TEST(ChunkRunSender, FullChunksCarryNoTrailingEmptyChunk)
{
  ChunkRecorder recorder;
  const Parts parts{{0, 1}, {2, 3}};
  ChunkRunSender sender(parts, /*max_items_per_chunk=*/2, recording(&recorder));
  sender.send()();
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 2U);
  expect_position(sent[0], 0, 2);
  EXPECT_EQ(sent[0].items, (Items{0, 1}));
  expect_position(sent[1], 1, 2);
  EXPECT_EQ(sent[1].items, (Items{2, 3}));
}

TEST(ChunkRunSender, TakesAnyContiguousContainerAsAPart)
{
  ChunkRecorder recorder;
  const std::vector<x23::XdrVec<std::uint32_t>> parts{{0, 1}, {2}};
  ChunkRunSender sender(parts, /*max_items_per_chunk=*/2, recording(&recorder));
  sender.send()();
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 2U);
  EXPECT_EQ(sent[0].items, (Items{0, 1}));
  EXPECT_EQ(sent[1].items, (Items{2}));
}

TEST(ChunkRunSender, EmptyInputStillMakesOneCall)
{
  for (const Parts &parts : {Parts{}, Parts{{}, {}}}) {
    ChunkRecorder recorder;
    ChunkRunSender sender(parts, /*max_items_per_chunk=*/3,
                          recording(&recorder));
    sender.send()();
    const std::vector<SentChunk> sent = recorder.sent();
    ASSERT_EQ(sent.size(), 1U);
    expect_position(sent[0], 0, 1);
    EXPECT_TRUE(sent[0].items.empty());
  }
}

namespace {

// A call that returns only once [expected] calls are in flight at once, so
// a run of that many chunks completes only if they were sent concurrently.
x23::Async<void>
await_peers(std::atomic<unsigned> *in_flight, unsigned expected)
{
  in_flight->fetch_add(1);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (in_flight->load() < expected) {
    if (std::chrono::steady_clock::now() >= deadline) {
      ADD_FAILURE() << "the run's chunks were not in flight together";
      co_return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    co_await default_coropoll().pool_.bg();
  }
  co_return;
}

} // namespace

TEST(ChunkRunSender, SendsChunksConcurrently)
{
  std::atomic<unsigned> in_flight{0};
  const Parts parts{{0, 1, 2, 3}};
  ChunkRunSender sender(parts, /*max_items_per_chunk=*/2,
                        [&in_flight](ChunkPosition /*position*/,
                                     ChunkItems<std::uint32_t> /*items*/) {
                          return await_peers(&in_flight, 2);
                        });
  sender.send(/*max_in_flight=*/2)();
  EXPECT_EQ(in_flight.load(), 2U);
}

TEST(ChunkRunSender, FailsWithACallsExceptionAndIssuesNoFurtherChunk)
{
  ChunkRecorder recorder;
  auto failing_second = [&recorder](ChunkPosition position,
                                    ChunkItems<std::uint32_t> items) {
    if (position.index == 1) {
      throw std::runtime_error("chunk refused");
    }
    return record(&recorder, position, items);
  };
  // One worker sends the chunks in order, so the failure on chunk 1 is what
  // stops chunk 2.
  const Parts parts{{0, 1, 2}};
  ChunkRunSender sender(parts, /*max_items_per_chunk=*/1, failing_second);
  EXPECT_THROW(sender.send(/*max_in_flight=*/1)(), std::runtime_error);
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 1U);
  expect_position(sent[0], 0, 3);
}

namespace {

// A call whose reply arrives from a thread outside the pool, the way a reply a
// connection's reader decodes resumes the caller on the reader's thread.
x23::Async<void>
reply_off_pool(std::vector<std::jthread> *repliers, std::mutex *repliers_mutex)
{
  x23::Awaitable<void> reply;
  reply.launch();
  {
    const std::lock_guard<std::mutex> lock(*repliers_mutex);
    repliers->emplace_back([&reply] { reply.set_result(); });
  }
  co_await reply;
}

} // namespace

TEST(ChunkRunSender, EveryChunkIsSentFromThePoolWhateverThreadRepliesArriveOn)
{
  std::atomic<unsigned> calls{0};
  std::atomic<unsigned> calls_on_pool{0};
  std::vector<std::jthread> repliers;
  std::mutex repliers_mutex;
  // Eight chunks over two workers: six of the calls follow a reply that came
  // in on a replier's thread, and each must still start on the pool.
  const Parts parts{{0, 1, 2, 3, 4, 5, 6, 7}};
  ChunkRunSender sender(parts, /*max_items_per_chunk=*/1,
                        [&calls, &calls_on_pool, &repliers,
                         &repliers_mutex](ChunkPosition /*position*/,
                                          ChunkItems<std::uint32_t> /*items*/) {
                          calls.fetch_add(1);
                          if (x23::in_thread_pool()) {
                            calls_on_pool.fetch_add(1);
                          }
                          return reply_off_pool(&repliers, &repliers_mutex);
                        });
  sender.send(/*max_in_flight=*/2)();
  repliers.clear();
  EXPECT_EQ(calls.load(), 8U);
  EXPECT_EQ(calls_on_pool.load(), 8U);
}

} // namespace hogd
