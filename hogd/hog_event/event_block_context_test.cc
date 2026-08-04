#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "event.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/string.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/event_block_context.h"
#include "hogd/hog_event/test_util/socketpair_indexer_client.h"
#include "hogd/init.h"
#include "indexer.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/fileio.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

class EventBlockContextTest : public ::testing::Test {
public:
  static std::unordered_map<SHA256Hash, TxEventLog, ByteArrayHasher>
  parse_event_logs(std::string_view filename)
  {
    std::unordered_map<SHA256Hash, TxEventLog, ByteArrayHasher> event_logs;
    auto buf = x23::readfile(filename);
    if (!buf) {
      panic("failed to read file: {}", filename);
    }

    size_t offset = 0;
    TxEventLog log;
    auto data = std::span(*buf);

    while (offset < buf->size()) {
      size_t n =
          x23::deserialize_from(&log, x23::BufView(data.subspan(offset)));
      offset += n;
      auto it = event_logs.insert({log.proposed_tx_hash, log});
      EXPECT_TRUE(it.second);
      if (!it.second) {
        error("duplicate event log for proposed tx hash: {}",
              encode_hex(std::as_bytes(std::span(log.proposed_tx_hash))));
      }
    }
    EXPECT_EQ(offset, buf->size());

    return event_logs;
  }

  TmpDir tmp_dir_;
  std::string temp_dir_ = tmp_dir_.directory();
};

namespace {
void
finish_file(EventBlockContext &context)
{
  context.finish_file_write()();
  context.finish_file(context.get_root_hash())();
}

} // namespace

TEST_F(EventBlockContextTest, BasicTest)
{
  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();
  EXPECT_TRUE(
      context.add_event(TxEventLog{.proposed_tx_hash =
                                       sha256(std::as_bytes(std::span("test"))),
                                   .events = {TxEmittedEvent{
                                       .contract_address = {"test_address"},
                                       .serialized_event = {1, 2, 3},
                                   }}},
                        ProposedTransaction{})());
  context.executor_close_block();

  finish_file(context);
  auto hash = context.get_root_hash();
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir_, hash)));

  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));
  EXPECT_EQ(event_logs.size(), 1);
  EXPECT_EQ(event_logs.begin()->first,
            sha256(std::as_bytes(std::span("test"))));
  EXPECT_EQ(event_logs.begin()->second.events.size(), 1);
  EXPECT_EQ(event_logs.begin()->second.events[0].contract_address,
            ContractAddress{"test_address"});
}

TEST_F(EventBlockContextTest, TestEmptyBlockWithExecutor)
{
  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();
  context.executor_close_block();
  finish_file(context);
  auto hash = context.get_root_hash();
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir_, hash)));
  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));
}

TEST_F(EventBlockContextTest, TestEmptyBlockNoExecutor)
{
  EventBlockContext context(temp_dir_, 1);
  finish_file(context);
  auto hash = context.get_root_hash();
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir_, hash)));
  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));
}

// What happens if there's a conflict on a filename
TEST_F(EventBlockContextTest, TestDuplicateEventBlocks)
{
  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();
  EXPECT_TRUE(
      context.add_event(TxEventLog{.proposed_tx_hash =
                                       sha256(std::as_bytes(std::span("test"))),
                                   .events = {TxEmittedEvent{
                                       .contract_address = {"test_address"},
                                       .serialized_event = {1, 2, 3},
                                   }}},
                        ProposedTransaction{})());
  context.executor_close_block();
  finish_file(context);
  auto hash = context.get_root_hash();
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir_, hash)));
  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));
  EXPECT_EQ(event_logs.size(), 1);
  EXPECT_EQ(event_logs.begin()->first,
            sha256(std::as_bytes(std::span("test"))));

  EventBlockContext context2(temp_dir_, 2);
  context2.executor_open_block();
  EXPECT_TRUE(context2.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"test_address"},
                     .serialized_event = {1, 2, 3},
                 }}},
      ProposedTransaction{})());
  context2.executor_close_block();
  finish_file(context2);
  auto hash2 = context2.get_root_hash();
  EXPECT_EQ(hash, hash2);
  auto event_logs2 = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash2));
  EXPECT_EQ(event_logs2.size(), 1);
  EXPECT_EQ(event_logs2.begin()->first,
            sha256(std::as_bytes(std::span("test"))));
  EXPECT_EQ(event_logs2.begin()->second.events.size(), 1);
  EXPECT_EQ(event_logs2.begin()->second.events[0].contract_address,
            ContractAddress{"test_address"});

  EXPECT_EQ(EventBlockContext::final_events_filename(temp_dir_, hash),
            EventBlockContext::final_events_filename(temp_dir_, hash2));
}

TEST_F(EventBlockContextTest, DifferentBlocksHaveDifferentHashes)
{
  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();
  EXPECT_TRUE(
      context.add_event(TxEventLog{.proposed_tx_hash =
                                       sha256(std::as_bytes(std::span("test"))),
                                   .events = {TxEmittedEvent{
                                       .contract_address = {"test_address"},
                                       .serialized_event = {1, 2, 3},
                                   }}},
                        ProposedTransaction{})());
  context.executor_close_block();
  finish_file(context);
  auto hash = context.get_root_hash();
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir_, hash)));
  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));
  EXPECT_EQ(event_logs.size(), 1);
  EXPECT_EQ(event_logs.begin()->first,
            sha256(std::as_bytes(std::span("test"))));

  EventBlockContext context2(temp_dir_, 2);
  context2.executor_open_block();
  EXPECT_TRUE(context2.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"address_test"},
                     .serialized_event = {1, 2, 3},
                 }}},
      ProposedTransaction{})());
  context2.executor_close_block();
  finish_file(context2);
  auto hash2 = context2.get_root_hash();
  EXPECT_NE(hash, hash2);
  auto event_logs2 = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash2));
  EXPECT_EQ(event_logs2.size(), 1);
  EXPECT_EQ(event_logs2.begin()->first,
            sha256(std::as_bytes(std::span("test"))));
  EXPECT_EQ(event_logs2.begin()->second.events.size(), 1);
  EXPECT_EQ(event_logs2.begin()->second.events[0].contract_address,
            ContractAddress{"address_test"});

  EXPECT_NE(EventBlockContext::final_events_filename(temp_dir_, hash),
            EventBlockContext::final_events_filename(temp_dir_, hash2));
}

// Test that abandoning a block and then trying to write an event doesn't crash
TEST_F(EventBlockContextTest, AbandonThenAddEvent)
{
  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();

  // Add an initial event to potentially trigger async buffer flushing
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("event1"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"test_address"},
                     .serialized_event = {1, 2, 3},
                 }}},
      ProposedTransaction{})());
  // Abandon the block - this should wait for any pending writes to complete
  context.await_abandon()();

  // Try to add an event after abandoning - this shouldn't crash
  // The event won't be written to the file (since state is kAbandoned),
  // but it should still update the trie and not panic.
  // open_executor_handles_ is still > 0 (abandon doesn't decrement it).
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("event2"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"test_address2"},
                     .serialized_event = {4, 5, 6},
                 }}},
      ProposedTransaction{})());

  // Verify the context can be destroyed without issues
  // (destructor asserts state is not kActive and pending_writes is 0)
}

// Test multiple executors opening and closing handles
TEST_F(EventBlockContextTest, MultipleExecutors)
{
  EventBlockContext context(temp_dir_, 1);

  // Three executors open handles
  context.executor_open_block();
  context.executor_open_block();
  context.executor_open_block();

  // Add some events
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test1"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"address1"},
                     .serialized_event = {1, 2, 3},
                 }}},
      ProposedTransaction{})());

  // First executor closes
  context.executor_close_block();

  // Add more events (2 executors still open)
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test2"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"address2"},
                     .serialized_event = {4, 5, 6},
                 }}},
      ProposedTransaction{})());

  // Second executor closes
  context.executor_close_block();

  // Third executor closes - should trigger all_executors_closed
  context.executor_close_block();

  // Verify await completes immediately since all are closed
  context.await_all_executors_closed()();

  finish_file(context);
  auto hash = context.get_root_hash();
  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));
  EXPECT_EQ(event_logs.size(), 2);
}

// Test that await_all_executors_closed waits for all executors
TEST_F(EventBlockContextTest, AwaitWaitsForAllExecutors)
{
  EventBlockContext context(temp_dir_, 1);

  // Open 2 executors
  context.executor_open_block();
  context.executor_open_block();

  // Spawn a task that waits for all executors
  std::atomic<bool> await_completed = false;
  auto async = [&context, &await_completed](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    co_await context.await_all_executors_closed();
    await_completed.store(true, std::memory_order_release);
  }();

  // Give the await task time to start waiting
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)

  // Await should not have completed yet
  EXPECT_FALSE(await_completed.load(std::memory_order_acquire));

  // Close first executor
  context.executor_close_block();
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)

  // Still should not be complete
  EXPECT_FALSE(await_completed.load(std::memory_order_acquire));

  // Close second executor - now await should complete
  context.executor_close_block();
  async();

  EXPECT_TRUE(await_completed.load(std::memory_order_acquire));

  finish_file(context);
}

// Test await when already all closed
TEST_F(EventBlockContextTest, AwaitWhenAlreadyClosed)
{
  EventBlockContext context(temp_dir_, 1);

  context.executor_open_block();
  context.executor_close_block();

  // Await should complete immediately
  context.await_all_executors_closed()();

  // Multiple awaits should all work
  context.await_all_executors_closed()();
  context.await_all_executors_closed()();

  finish_file(context);
}

// Test await when no executors ever opened
TEST_F(EventBlockContextTest, AwaitWithNoExecutors)
{
  EventBlockContext context(temp_dir_, 1);

  // Await should complete immediately when counter is 0
  context.await_all_executors_closed()();

  finish_file(context);
}

// Note: executor_close_block() CHECKs against underflow (a close without a
// matching open), in all build types -- block ids are never reused, so an
// unmatched close is an executor bug, and a wrapped count would wedge
// await_all_executors_closed() forever. Not practical to test with
// EXPECT_DEATH due to destructor assertions conflicting with the death test
// framework.

// Test multiple concurrent awaits (validates futex broadcast)
TEST_F(EventBlockContextTest, MultipleConcurrentAwaits)
{
  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();

  std::atomic<int> completed_awaits = 0;

  // Spawn 3 concurrent awaiters
  auto awaiter = [&context, &completed_awaits](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    co_await context.await_all_executors_closed();
    completed_awaits.fetch_add(1, std::memory_order_release);
  };

  auto async1 = awaiter();
  auto async2 = awaiter();
  auto async3 = awaiter();

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)
  EXPECT_EQ(completed_awaits.load(), 0);

  // Close executor - all 3 awaits should complete
  context.executor_close_block();

  async1();
  async2();
  async3();

  EXPECT_EQ(completed_awaits.load(), 3);

  finish_file(context);
}

// Stress test: blast events from 128 threads to exercise the concurrent buffer
// flush path in write_event (buffers_->buf >= 500 KiB).  This validates the
// fix for extracting the PerThread buffer before co_await — without the fix,
// write_event would resolve buffers_-> on the wrong thread after resuming.
//
// Note: the kFinishing data-preservation path (where write_event puts extracted
// data back into buffers_->buf) cannot be targeted here.  In valid usage,
// finish_file_write requires open_executor_handles_ == 0, so all add_event
// calls have completed before kFinishing is set.  The kFinishing guard in
// write_event is purely defensive.
TEST_F(EventBlockContextTest, StressWriteEventDuringFinish)
{
  // Use large payloads so each thread's PerThread buffer crosses
  // kBufferThreshold (500 KiB) quickly, forcing the flush code path.
  constexpr size_t kNumThreads = 128;
  constexpr size_t kEventsPerThread = 20;
  constexpr size_t kPayloadSize = 512;

  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();

  std::atomic<size_t> events_accepted{0};
  std::atomic<bool> any_event_accepted{false};

  // Each thread adds events with unique hashes and large payloads.
  std::vector<x23::Async<void>> producers;
  producers.reserve(kNumThreads);
  for (size_t t = 0; t < kNumThreads; t++) {
    producers.push_back([](EventBlockContext &context,
                           std::atomic<size_t> &events_accepted,
                           std::atomic<bool> &any_event_accepted,
                           size_t t) -> x23::Async<void> {
      co_await default_threadpool().bg();
      for (size_t i = 0; i < kEventsPerThread; i++) {
        uint64_t idx = (t * kEventsPerThread) + i;
        auto hash = sha256(std::as_bytes(std::span(&idx, 1)));
        x23::Vec<uint8_t, kMaxEventLength> payload;
        payload.resize(kPayloadSize);
        std::memcpy(payload.data(), &idx, sizeof(idx));

        TxEventLog event{
            .proposed_tx_hash = hash,
            .events = {TxEmittedEvent{
                .contract_address = {},
                .num_topics = 0,
                .serialized_event = payload,
            }},
        };
        if (co_await context.add_event(std::move(event),
                                       ProposedTransaction{})) {
          events_accepted.fetch_add(1, std::memory_order_relaxed);
          any_event_accepted.store(true, std::memory_order_release);
        }
      }
    }(context, events_accepted, any_event_accepted, t));
  }

  // Wait until at least one producer has successfully added an event before
  // closing the executor.  The bare sleep_for(1ms) was insufficient under
  // ASAN+Release where coroutine scheduling overhead could prevent any
  // producer from completing add_event within the window.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!any_event_accepted.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() > deadline) {
      FAIL() << "Timed out waiting for any producer to accept an event";
    }
    std::this_thread::yield();
  }

  context.executor_close_block();

  // Drain all producers.
  for (auto &p : producers) {
    p();
  }

  finish_file(context);
  auto hash = context.get_root_hash();

  // Parse the output and verify that every accepted event is present.
  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));

  size_t accepted = events_accepted.load(std::memory_order_relaxed);
  EXPECT_GT(accepted, 0UL);
  EXPECT_EQ(event_logs.size(), accepted);
}

// Regression test for stale all_writes_done_ futex (issue #1551).
// Forces two async flush epochs on a single-worker pool, then calls
// finish_file_write().  Without the fix, the stale futex causes a deadlock
// (ctest --timeout catches it).
TEST_F(EventBlockContextTest, TwoFlushEpochsDoNotDeadlock)
{
  x23::ThreadPool pool(1);
  EventBlockContext context(temp_dir_, 1);
  context.executor_open_block();

  [](x23::ThreadPool &pool, EventBlockContext &context) -> x23::Async<void> {
    co_await pool.bg();

    for (int i = 0; i < 600; i++) {
      SHA256Hash hash{};
      std::memcpy(hash.data(), &i, sizeof(i));
      x23::Vec<uint8_t, kMaxEventLength> payload;
      payload.resize(1024);
      std::memcpy(payload.data(), &i, sizeof(i));
      co_await context.add_event(
          TxEventLog{
              .proposed_tx_hash = hash,
              .events = {TxEmittedEvent{
                  .contract_address = {},
                  .num_topics = 0,
                  .serialized_event = std::move(payload),
              }},
          },
          ProposedTransaction{});
    }

    // Yield so the first flush epoch completes on the single worker.
    co_await pool.bg();

    for (int i = 600; i < 1200; i++) {
      SHA256Hash hash{};
      std::memcpy(hash.data(), &i, sizeof(i));
      x23::Vec<uint8_t, kMaxEventLength> payload;
      payload.resize(1024);
      std::memcpy(payload.data(), &i, sizeof(i));
      co_await context.add_event(
          TxEventLog{
              .proposed_tx_hash = hash,
              .events = {TxEmittedEvent{
                  .contract_address = {},
                  .num_topics = 0,
                  .serialized_event = std::move(payload),
              }},
          },
          ProposedTransaction{});
    }

    context.executor_close_block();
    co_await context.finish_file_write();
    co_await context.finish_file(context.get_root_hash());
  }(pool, context)();
}

namespace {
struct CapturingIndexerRpc {
  using rpc_service = IndexerEvent1;

  x23::Async<void>
  indexer_event_process_events(BlockHeight block_height,
                               const TxEventLogBatch &tx_events,
                               const ProposedTransactionBatch &tx_bodies)
  {
    received_batches++;
    last_block_height = block_height;
    received_events = tx_events;
    received_bodies = tx_bodies;
    co_return;
  }

  // Required for is_IndexerEvent1 conformance; the failure-relay path is
  // covered by FailureCapturingIndexerRpc in event_rpc_test.cc, so this file's
  // tests don't record reports.
  static x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash & /*tx_hash*/,
                                  const TxFailureDiagnostic & /*diag*/)
  {
    co_return;
  }

  size_t received_batches = 0;
  BlockHeight last_block_height = -1;
  TxEventLogBatch received_events;
  ProposedTransactionBatch received_bodies;
};
static_assert(is_IndexerEvent1<CapturingIndexerRpc *, x23::Async>);

ProposedTransaction
make_distinct_body(uint64_t nonce)
{
  ProposedTransaction body;
  body.signed_tx.tx.uniquifier.block_expiry_height =
      static_cast<BlockHeight>(1000 + nonce);
  body.signed_tx.tx.uniquifier.nonce = nonce;
  body.signed_tx.tx.invocation.gas_limit = 1'000'000 + nonce;
  return body;
}

TxEventLog
make_event_for_body(ProposedTransaction const &body, uint8_t marker)
{
  TxEmittedEvent emitted_event;
  emitted_event.contract_address[0] = marker;
  emitted_event.serialized_event.push_back(marker);

  TxEventLog event;
  event.proposed_tx_hash = sha256(body.signed_tx.tx);
  event.events.push_back(std::move(emitted_event));
  return event;
}
} // namespace

TEST_F(EventBlockContextTest, IndexerFanoutCanMispairPerThreadEventBodies)
{
  EventBlockContext context(temp_dir_, 1);
  PerThread<TxEventLogBatch> event_registration_probe;
  PerThread<ProposedTransactionBatch> body_registration_probe;

  auto body_a = make_distinct_body(101);
  auto body_b = make_distinct_body(202);
  auto event_a = make_event_for_body(body_a, 0xA1);
  auto event_b = make_event_for_body(body_b, 0xB2);

  std::promise<void> a_event_registered;
  std::promise<void> b_body_registered;
  std::promise<void> a_body_registered;
  std::promise<void> start_add_a;
  std::promise<void> start_add_b;
  std::promise<void> done_add_a;
  std::promise<void> done_add_b;
  std::promise<void> release_workers;

  auto a_event_registered_f = a_event_registered.get_future();
  auto b_body_registered_f = b_body_registered.get_future();
  auto a_body_registered_f = a_body_registered.get_future();
  auto start_add_a_f = start_add_a.get_future();
  auto start_add_b_f = start_add_b.get_future();
  auto done_add_a_f = done_add_a.get_future();
  auto done_add_b_f = done_add_b.get_future();
  auto release_workers_f = release_workers.get_future().share();

  std::atomic<bool> added_a = false;
  std::atomic<bool> added_b = false;

  std::thread worker_a([&] {
    event_registration_probe->clear();
    a_event_registered.set_value();
    b_body_registered_f.wait();
    body_registration_probe->clear();
    a_body_registered.set_value();

    start_add_a_f.wait();
    added_a.store(context.add_event(event_a, body_a)(),
                  std::memory_order_release);
    done_add_a.set_value();
    release_workers_f.wait();
  });

  std::thread worker_b([&] {
    a_event_registered_f.wait();
    event_registration_probe->clear();
    body_registration_probe->clear();
    b_body_registered.set_value();

    start_add_b_f.wait();
    added_b.store(context.add_event(event_b, body_b)(),
                  std::memory_order_release);
    done_add_b.set_value();
    release_workers_f.wait();
  });

  a_body_registered_f.wait();

  context.executor_open_block();
  start_add_a.set_value();
  done_add_a_f.wait();
  start_add_b.set_value();
  done_add_b_f.wait();
  EXPECT_TRUE(added_a.load(std::memory_order_acquire));
  EXPECT_TRUE(added_b.load(std::memory_order_acquire));
  context.executor_close_block();
  finish_file(context);

  CapturingIndexerRpc indexer;
  std::vector<indexer_event_client_t> indexers;
  indexers.push_back(make_socketpair_indexer_client(&indexer));
  try {
    context.send_events_to_indexers(/*block_height=*/7, indexers)();
  } catch (...) {
    release_workers.set_value();
    worker_a.join();
    worker_b.join();
    throw;
  }

  release_workers.set_value();
  worker_a.join();
  worker_b.join();

  ASSERT_EQ(indexer.received_batches, 1);
  ASSERT_EQ(indexer.last_block_height, 7);
  ASSERT_EQ(indexer.received_events.size(), 2);
  ASSERT_EQ(indexer.received_bodies.size(), 2);

  // For every forwarded index i, the body must be the one that produced the
  // event log at the same index.
  for (size_t i = 0; i < indexer.received_events.size(); i++) {
    EXPECT_EQ(sha256(indexer.received_bodies[i].signed_tx.tx),
              indexer.received_events[i].proposed_tx_hash);
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
