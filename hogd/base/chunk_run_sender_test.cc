#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
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
#include "hogd/base/types.h"
#include "x23/async.h"
#include "x23/threadpool.h"

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
  ChunkRunSender sender(Parts{{0, 1, 2}, {3, 4}, {}, {5, 6}},
                        /*max_items_per_chunk=*/3, recording(&recorder));
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
  ChunkRunSender sender(Parts{{0, 1}, {2, 3}}, /*max_items_per_chunk=*/2,
                        recording(&recorder));
  sender.send()();
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 2U);
  expect_position(sent[0], 0, 2);
  EXPECT_EQ(sent[0].items, (Items{0, 1}));
  expect_position(sent[1], 1, 2);
  EXPECT_EQ(sent[1].items, (Items{2, 3}));
}

TEST(ChunkRunSender, EmptyInputStillMakesOneCall)
{
  for (Parts parts : {Parts{}, Parts{{}, {}}}) {
    ChunkRecorder recorder;
    ChunkRunSender sender(std::move(parts), /*max_items_per_chunk=*/3,
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
  ChunkRunSender sender(Parts{{0, 1, 2, 3}}, /*max_items_per_chunk=*/2,
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
  ChunkRunSender sender(Parts{{0, 1, 2}}, /*max_items_per_chunk=*/1,
                        failing_second);
  EXPECT_THROW(sender.send(/*max_in_flight=*/1)(), std::runtime_error);
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 1U);
  expect_position(sent[0], 0, 3);
}

namespace {

// A call the test releases: it reports that it has started, awaits its
// release, which arrives from off the pool, and then fails if [fail].
struct Gate {
  Gate() { reply.launch(); }

  std::atomic<bool> started{false};
  x23::Awaitable<void> reply;
};

x23::Async<void>
gated(Gate *gate, bool fail)
{
  gate->started.store(true);
  co_await gate->reply;
  if (fail) {
    throw std::runtime_error("chunk refused");
  }
}

// Occupies one pool thread until [release] opens, counting itself in
// [holding] once it has the thread.
x23::Async<void>
hold_pool_thread(std::atomic<size_t> *holding, std::latch *release)
{
  co_await default_coropoll().pool_.bg();
  holding->fetch_add(1);
  release->wait();
}

// Polls until [ready] holds, for at most ten seconds. On timeout it fails the
// test, naming [what] it waited for, and returns false.
bool
wait_for(const auto &ready, const char *what)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      ADD_FAILURE() << "timed out waiting for " << what;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

} // namespace

TEST(ChunkRunSender, IssuesNoChunkAfterAFailureEvenFromAWorkerMidHop)
{
  // Three one-item chunks over two workers. Chunk 0's call fails and chunk 1's
  // call returns, each only when the test releases it. Before releasing chunk
  // 1, the test fills the pool with blocked tasks, so chunk 1's worker takes
  // chunk 2 and parks in its pool hop, past the loop's failure check. Chunk 0
  // then fails. Once the pool is freed, that worker must drop chunk 2 rather
  // than issue it.
  Gate gate0;
  Gate gate1;
  ChunkRecorder issued;
  ChunkRunSender sender(Parts{{0, 1, 2}}, /*max_items_per_chunk=*/1,
                        [&gate0, &gate1,
                         &issued](ChunkPosition position,
                                  ChunkItems<std::uint32_t> items) {
                          switch (position.index) {
                          case 0:
                            return gated(&gate0, /*fail=*/true);
                          case 1:
                            return gated(&gate1, /*fail=*/false);
                          default:
                            return record(&issued, position, items);
                          }
                        });
  std::atomic<bool> failed{false};
  std::jthread run([&sender, &failed] {
    try {
      sender.send(/*max_in_flight=*/2)();
    } catch (const std::runtime_error &) {
      failed.store(true);
    }
  });

  const size_t nthreads = default_coropoll().pool_.nthreads_;
  std::atomic<size_t> holding{0};
  std::latch release(1);
  std::vector<x23::Async<void>> holders;
  bool released0 = false;
  bool released1 = false;
  bool released_pool = false;
  // Frees everything the test holds, so a failed wait ends the test instead of
  // hanging it.
  const auto release_all = [&] {
    if (!released1) {
      gate1.reply.set_result();
    }
    if (!released0) {
      gate0.reply.set_result();
    }
    if (!released_pool) {
      release.count_down();
    }
    for (x23::Async<void> &holder : holders) {
      holder();
    }
    run.join();
  };

  if (!wait_for([&] { return gate0.started.load() && gate1.started.load(); },
                "both calls to be in flight")) {
    release_all();
    return;
  }
  for (size_t i = 0; i < nthreads; ++i) {
    holders.push_back(hold_pool_thread(&holding, &release));
  }
  if (!wait_for([&] { return holding.load() == nthreads; },
                "the pool to fill")) {
    release_all();
    return;
  }

  // Chunk 1 completes off the pool; its worker takes chunk 2 and hops, but
  // the pool is full, so it parks there.
  {
    const std::jthread replier([&gate1] { gate1.reply.set_result(); });
  }
  released1 = true;
  EXPECT_TRUE(issued.sent().empty());

  // Chunk 0 fails while that worker is parked.
  {
    const std::jthread replier([&gate0] { gate0.reply.set_result(); });
  }
  released0 = true;

  release.count_down();
  released_pool = true;
  release_all();
  EXPECT_TRUE(failed.load());
  EXPECT_TRUE(issued.sent().empty());
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
  ChunkRunSender sender(Parts{{0, 1, 2, 3, 4, 5, 6, 7}},
                        /*max_items_per_chunk=*/1,
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
