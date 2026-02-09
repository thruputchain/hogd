#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "event.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep#include "hogd/crypto/crypto.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/event_block_context.h"
#include "hogd/init.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/fileio.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

class EventBlockContextTest : public ::testing::Test {
public:
  EventBlockContextTest() = default;

  void SetUp() override { temp_dir_ = TmpDir().directory(); }

  void TearDown() override { std::filesystem::remove_all(temp_dir_); }

  static std::unordered_map<SHA256Hash, TxEventLog>
  parse_event_logs(std::string_view filename)
  {
    std::unordered_map<SHA256Hash, TxEventLog> event_logs;
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

  std::string temp_dir_;
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
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"test_address"},
                     .serialized_event = {1, 2, 3},
                 }}})());
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
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"test_address"},
                     .serialized_event = {1, 2, 3},
                 }}})());
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
                 }}})());
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
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"test_address"},
                     .serialized_event = {1, 2, 3},
                 }}})());
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
                 }}})());
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
                 }}})());
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
                 }}})());

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
                 }}})());

  // First executor closes
  context.executor_close_block();

  // Add more events (2 executors still open)
  EXPECT_TRUE(context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test2"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"address2"},
                     .serialized_event = {4, 5, 6},
                 }}})());

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

// Note: We have an assertion in executor_close_block() that prevents underflow
// (calling close without a matching open), but it's not practical to test with
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
