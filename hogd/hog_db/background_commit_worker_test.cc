#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "auth_rpc.hh"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "hogd/hog_db/background_commit_worker.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/sockio.h"

namespace hogd {
namespace {

// Records every chunk the worker sends, in arrival order.
class ChunkRecordingSchedDbRpc {
public:
  using rpc_service = SchedDb1;

  void scheddb_intro_shard(ShardCard const & /*unused*/) {}
  void scheddb_hogs_transferred_chunk(HogsTransferredChunk const &chunk)
  {
    std::lock_guard<std::mutex> lock(mtx_);
    chunks_.push_back(chunk);
  }

  std::vector<HogsTransferredChunk> chunks()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return chunks_;
  }

private:
  std::mutex mtx_;
  std::vector<HogsTransferredChunk> chunks_;
};

static_assert(is_SchedDb1<ChunkRecordingSchedDbRpc *, std::type_identity_t>);

Delta
make_delta(uint64_t i)
{
  Delta delta{.address = {}, .hog_delta = to_xdr(static_cast<int128_t>(i) + 1)};
  for (size_t byte = 0; byte < sizeof(i); ++byte) {
    delta.address[byte] = static_cast<uint8_t>(i >> (8 * byte));
  }
  return delta;
}

// Runs a block of [num_deltas] deltas through a worker that chunks at
// [max_deltas_per_chunk] and returns what the scheduler side received.
std::vector<HogsTransferredChunk>
run_block(size_t num_deltas, size_t max_deltas_per_chunk,
          size_t expected_chunks)
{
  RpcServer<ChunkRecordingSchedDbRpc> server;
  const uint16_t port = server.start_server(x23::HostPort{"127.0.0.1", 0});
  auto client = make_rpc_client<SchedDb1>(x23::HostPort{"127.0.0.1", port},
                                          /*retry=*/false)();
  EXPECT_TRUE(client.has_value());
  if (!client.has_value()) {
    return {};
  }
  auto shared_client =
      std::make_shared<BackgroundCommitWorker::sched_db_client_t>(
          std::move(*client));

  AuthChangeSubscriptionManager subscription_manager;
  BackgroundCommitWorker worker(subscription_manager, shared_client,
                                max_deltas_per_chunk);

  PerThread<std::vector<Delta>> balance_deltas;
  for (uint64_t i = 0; i < num_deltas; ++i) {
    balance_deltas->push_back(make_delta(i));
  }
  PerThread<KeyChangeBatch> key_changes;
  worker.submit_job(/*block_height=*/9, std::move(key_changes),
                    std::move(balance_deltas));

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (server.impl()->chunks().size() < expected_chunks) {
    EXPECT_LT(std::chrono::steady_clock::now(), deadline)
        << "the scheduler side never received the run";
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Give a stray extra chunk a moment to show up before counting.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return server.impl()->chunks();
}

// Checks [arrived] is the whole run, split at [max_deltas_per_chunk], in
// whatever order the chunks were sent.
void
expect_run(std::vector<HogsTransferredChunk> arrived, size_t num_deltas,
           size_t max_deltas_per_chunk)
{
  ASSERT_FALSE(arrived.empty());
  std::vector<HogsTransferredChunk> chunks = std::move(arrived);
  std::ranges::sort(chunks, {}, [](const HogsTransferredChunk &chunk) {
    return chunk.chunk_index;
  });
  const auto chunk_count = static_cast<uint32_t>(chunks.size());
  uint64_t next = 0;
  for (uint32_t index = 0; index < chunk_count; ++index) {
    const HogsTransferredChunk &chunk = chunks[index];
    EXPECT_EQ(chunk.block, 9);
    EXPECT_EQ(chunk.chunk_index, index);
    EXPECT_EQ(chunk.chunk_count, chunk_count);
    EXPECT_LE(chunk.deltas.size(), max_deltas_per_chunk);
    // Every chunk but the last is full.
    if (index + 1 < chunk_count) {
      EXPECT_EQ(chunk.deltas.size(), max_deltas_per_chunk);
    }
    for (const Delta &delta : chunk.deltas) {
      EXPECT_EQ(from_xdr(delta.hog_delta), static_cast<int128_t>(next + 1));
      EXPECT_EQ(delta.address[0], static_cast<uint8_t>(next));
      ++next;
    }
  }
  EXPECT_EQ(next, num_deltas);
}

} // namespace

TEST(BackgroundCommitWorker, SplitsABlockIntoAnIndexedRunOfChunks)
{
  const auto chunks = run_block(/*num_deltas=*/7, /*max_deltas_per_chunk=*/3,
                                /*expected_chunks=*/3);
  ASSERT_EQ(chunks.size(), 3U);
  expect_run(chunks, 7, 3);
}

TEST(BackgroundCommitWorker, FullChunksCarryNoTrailingEmptyChunk)
{
  const auto chunks = run_block(/*num_deltas=*/6, /*max_deltas_per_chunk=*/3,
                                /*expected_chunks=*/2);
  ASSERT_EQ(chunks.size(), 2U);
  expect_run(chunks, 6, 3);
}

TEST(BackgroundCommitWorker, EmptyBlockStillSendsOneLastChunk)
{
  const auto chunks = run_block(/*num_deltas=*/0, /*max_deltas_per_chunk=*/3,
                                /*expected_chunks=*/1);
  ASSERT_EQ(chunks.size(), 1U);
  EXPECT_EQ(chunks[0].block, 9);
  EXPECT_EQ(chunks[0].chunk_index, 0U);
  EXPECT_EQ(chunks[0].chunk_count, 1U);
  EXPECT_TRUE(chunks[0].deltas.empty());
}

} // namespace hogd
