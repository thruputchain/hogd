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
#include "hogd/base/chunked_call.h"
#include "hogd/base/coropoll.h"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

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

TEST(CallInChunks, NumbersItemsAcrossPartsIntoChunksOfAtMostTheLimit)
{
  ChunkRecorder recorder;
  call_in_chunks<std::uint32_t>(Parts{{0, 1, 2}, {3, 4}, {}, {5, 6}},
                                /*max_items_per_chunk=*/3,
                                recording(&recorder))();
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 3U);
  expect_position(sent[0], 0, 3);
  EXPECT_EQ(sent[0].items, (Items{0, 1, 2}));
  expect_position(sent[1], 1, 3);
  EXPECT_EQ(sent[1].items, (Items{3, 4, 5}));
  expect_position(sent[2], 2, 3);
  EXPECT_EQ(sent[2].items, (Items{6}));
}

TEST(CallInChunks, FullChunksCarryNoTrailingEmptyChunk)
{
  ChunkRecorder recorder;
  call_in_chunks<std::uint32_t>(Parts{{0, 1}, {2, 3}},
                                /*max_items_per_chunk=*/2,
                                recording(&recorder))();
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 2U);
  expect_position(sent[0], 0, 2);
  EXPECT_EQ(sent[0].items, (Items{0, 1}));
  expect_position(sent[1], 1, 2);
  EXPECT_EQ(sent[1].items, (Items{2, 3}));
}

TEST(CallInChunks, EmptyInputStillMakesOneCall)
{
  for (Parts parts : {Parts{}, Parts{{}, {}}}) {
    ChunkRecorder recorder;
    call_in_chunks<std::uint32_t>(std::move(parts), /*max_items_per_chunk=*/3,
                                  recording(&recorder))();
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

TEST(CallInChunks, SendsChunksConcurrently)
{
  std::atomic<unsigned> in_flight{0};
  call_in_chunks<std::uint32_t>(
      Parts{{0, 1, 2, 3}}, /*max_items_per_chunk=*/2,
      [&in_flight](ChunkPosition /*position*/,
                   ChunkItems<std::uint32_t> /*items*/) {
        return await_peers(&in_flight, 2);
      },
      /*max_in_flight=*/2)();
  EXPECT_EQ(in_flight.load(), 2U);
}

TEST(CallInChunks, FailsWithACallsExceptionAndIssuesNoFurtherChunk)
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
  EXPECT_THROW(call_in_chunks<std::uint32_t>(Parts{{0, 1, 2}},
                                             /*max_items_per_chunk=*/1,
                                             failing_second,
                                             /*max_in_flight=*/1)(),
               std::runtime_error);
  const std::vector<SentChunk> sent = recorder.sent();
  ASSERT_EQ(sent.size(), 1U);
  expect_position(sent[0], 0, 3);
}

static_assert(x23::serializes_like<ChunkItems<std::uint32_t>,
                                   x23::XdrVec<std::uint32_t>>);

TEST(ChunkItems, SerializesLikeTheVectorOfItsItems)
{
  const Items first{1, 2, 3};
  const Items second{};
  const Items third{4};
  const std::vector<std::span<const std::uint32_t>> pieces{first, second,
                                                           third};
  const ChunkItems<std::uint32_t> items(pieces);
  const x23::XdrVec<std::uint32_t> vector{1, 2, 3, 4};
  EXPECT_EQ(items.size(), 4U);
  EXPECT_EQ(xdr_size(items), x23::xdr_size(vector));
  EXPECT_EQ(x23::serialize(items), x23::serialize(vector));
}

TEST(ChunkItems, SerializesVariableSizeItemsLikeTheirVector)
{
  using Inner = x23::XdrVec<std::uint32_t>;
  const std::vector<Inner> first{Inner{1, 2}, Inner{}};
  const std::vector<Inner> second{Inner{3, 4, 5}};
  const std::vector<std::span<const Inner>> pieces{first, second};
  const ChunkItems<Inner> items(pieces);
  const x23::XdrVec<Inner> vector{Inner{1, 2}, Inner{}, Inner{3, 4, 5}};
  EXPECT_EQ(xdr_size(items), x23::xdr_size(vector));
  EXPECT_EQ(x23::serialize(items), x23::serialize(vector));
}

TEST(ChunkItems, EmptyChunkSerializesLikeAnEmptyVector)
{
  const std::vector<std::span<const std::uint32_t>> pieces;
  const ChunkItems<std::uint32_t> items(pieces);
  EXPECT_EQ(x23::serialize(items),
            x23::serialize(x23::XdrVec<std::uint32_t>{}));
}

} // namespace hogd
