#include <algorithm>
#include <gtest/gtest.h>

#include <cstddef> // NOLINT(misc-include-cleaner)
#include <cstdint> // NOLINT(misc-include-cleaner)
#include <filesystem>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

#include "event.hh"
#include "hogd/base/test.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/event_cursor.h"
#include "hogd/indexer/status.h"
#include "indexer.hh"
#include "pagination.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

// basic tests on open/close db ops
// NOLINTNEXTLINE(misc-use-internal-linkage)
class IndexDBTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // Ensure the test directory exists for the database under test
    std::filesystem::create_directories(tmp_dir_.directory());
  }

  TmpDir tmp_dir_;
};

// Test creating a new database
TEST_F(IndexDBTest, CreateNew)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());
  EXPECT_TRUE(db->is_open());
}

TEST_F(IndexDBTest, ClampEventsLimitDefaultsAndCaps)
{
  EXPECT_EQ(IndexDB::clamp_events_limit(0), IndexDB::kEventsDefaultLimit);
  EXPECT_EQ(IndexDB::clamp_events_limit(1), 1U);
  EXPECT_EQ(IndexDB::clamp_events_limit(IndexDB::kEventsMaxLimit),
            IndexDB::kEventsMaxLimit);
  EXPECT_EQ(IndexDB::clamp_events_limit(IndexDB::kEventsMaxLimit + 1),
            IndexDB::kEventsMaxLimit);
  EXPECT_EQ(IndexDB::clamp_events_limit(1'000'000), IndexDB::kEventsMaxLimit);
}

// Test that creating a new database fails if one already exists
TEST_F(IndexDBTest, CreateNewFailsIfExists)
{
  {
    auto db1 = IndexDB::make_new(tmp_dir_.directory());
    ASSERT_TRUE(db1.has_value());
  }

  // Try to create another database at the same path (should fail)
  auto db2 = IndexDB::make_new(tmp_dir_.directory());
  EXPECT_FALSE(db2.has_value());
  EXPECT_EQ(db2.error(), IndexerErrorCode::kDBAlreadyExists);
}

// Test opening an existing database
TEST_F(IndexDBTest, OpenExisting)
{
  // Create a database first
  {
    auto db = IndexDB::make_new(tmp_dir_.directory());
    ASSERT_TRUE(db.has_value());
  }

  // Now open it
  auto db = IndexDB::open(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());
  EXPECT_TRUE(db->is_open());
}

// Test that opening a non-existent database fails
TEST_F(IndexDBTest, OpenNonExistent)
{
  auto db = IndexDB::open(tmp_dir_.directory());
  EXPECT_FALSE(db.has_value());
  EXPECT_EQ(db.error(), IndexerErrorCode::kDBNotFound);
}

// Test consuming events and looking them up by transaction
TEST_F(IndexDBTest, ConsumeAndLookupTxEvents)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  // Create test events
  BlockHeight block_height = 100;
  SHA256Hash tx_hash1, tx_hash2;
  std::ranges::fill(tx_hash1, 0x01);
  std::ranges::fill(tx_hash2, 0x02);

  ContractAddress contract_addr1, contract_addr2;
  std::ranges::fill(contract_addr1, 0xAA);
  std::ranges::fill(contract_addr2, 0xBB);

  TxEventLog event_log1;
  event_log1.proposed_tx_hash = tx_hash1;
  TxEmittedEvent event1;
  event1.contract_address = contract_addr1;
  event1.num_topics = 0;
  event1.serialized_event.resize(10);
  std::ranges::fill(event1.serialized_event, 0x11);
  event_log1.events.push_back(event1);

  TxEventLog event_log2;
  event_log2.proposed_tx_hash = tx_hash2;
  TxEmittedEvent event2;
  event2.contract_address = contract_addr2;
  event2.num_topics = 1;
  event2.serialized_event.resize(20);
  std::ranges::fill(event2.serialized_event, 0x22);
  event_log2.events.push_back(event2);

  std::vector<TxEventLog> events = {event_log1, event_log2};

  // Consume events
  auto result = db->consume_events(block_height, events, {});
  ASSERT_TRUE(result.has_value());

  // Lookup first transaction
  auto lookup1 = db->get_tx_events(tx_hash1);
  ASSERT_TRUE(lookup1.has_value());
  EXPECT_EQ(lookup1->first, block_height);
  EXPECT_EQ(lookup1->second.proposed_tx_hash, tx_hash1);
  EXPECT_EQ(lookup1->second.events.size(), 1);
  EXPECT_EQ(lookup1->second.events[0].contract_address, contract_addr1);

  // Lookup second transaction
  auto lookup2 = db->get_tx_events(tx_hash2);
  ASSERT_TRUE(lookup2.has_value());
  EXPECT_EQ(lookup2->first, block_height);
  EXPECT_EQ(lookup2->second.proposed_tx_hash, tx_hash2);
  EXPECT_EQ(lookup2->second.events.size(), 1);
  EXPECT_EQ(lookup2->second.events[0].contract_address, contract_addr2);

  // Lookup non-existent transaction
  SHA256Hash tx_hash3;
  std::ranges::fill(tx_hash3, 0x03);
  auto lookup3 = db->get_tx_events(tx_hash3);
  EXPECT_FALSE(lookup3.has_value());
  EXPECT_EQ(lookup3.error(), IndexerErrorCode::kDBNotFound);

  // Test is_tx_included
  auto included1 = db->get_tx_events(tx_hash1);
  ASSERT_TRUE(included1.has_value());

  auto included3 = db->get_tx_events(tx_hash3);
  ASSERT_FALSE(included3.has_value());
  EXPECT_EQ(included3.error(), IndexerErrorCode::kDBNotFound);
}

// -------------------------------------------------------------------
// get_events tests
// -------------------------------------------------------------------

// NOLINTBEGIN(misc-include-cleaner)
namespace {

// Build a TxEmittedEvent with num_topics topics (each uniformly filled
// with the given byte), plus 4 bytes of payload.  serialized_event
// layout is num_topics * 32 bytes of topic data followed by payload.
TxEmittedEvent
make_event(ContractAddress const &contract, uint32_t num_topics,
           uint8_t topic_fill_start)
{
  TxEmittedEvent e;
  e.contract_address = contract;
  e.num_topics = num_topics;
  e.serialized_event.resize((num_topics * 32) + 4);
  for (uint32_t t = 0; t < num_topics; ++t) {
    for (size_t i = 0; i < 32; ++i) {
      e.serialized_event[(t * 32) + i] = topic_fill_start + t;
    }
  }
  // Payload bytes
  for (size_t i = 0; i < 4; ++i) {
    e.serialized_event[(num_topics * 32) + i] = 0xAB;
  }
  return e;
}

Topic
topic_filled(uint8_t fill)
{
  Topic t;
  std::ranges::fill(t, fill);
  return t;
}

// Build an EventQuery with sensible defaults.
EventQuery
make_query(EventFilterList filters, IterationOrder order,
           BlockAnchor const &anchor, uint32_t limit = 50)
{
  EventQuery q;
  q.filters = std::move(filters);
  q.order = order;
  q.anchor = anchor;
  q.limit = limit;
  return q;
}

BlockAnchor
anchor_at_height(BlockHeight h)
{
  BlockAnchor a(BlockAnchorKind::AnchorAtHeight);
  a.height() = h;
  return a;
}

EventFilter
filter_with_contracts_no_topics(std::initializer_list<ContractAddress> addrs)
{
  EventFilter f;
  for (auto const &a : addrs) {
    f.contract_addresses.push_back(a);
  }
  return f;
}

} // namespace

TEST_F(IndexDBTest, GetEventsSingleFilterAscending)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  SHA256Hash tx1, tx2;
  std::ranges::fill(tx1, 0x01);
  std::ranges::fill(tx2, 0x02);

  // Block 10: one event; block 11: two events.
  TxEventLog event_log1;
  event_log1.proposed_tx_hash = tx1;
  event_log1.events.push_back(make_event(addr, 0, 0));

  TxEventLog event_log2;
  event_log2.proposed_tx_hash = tx2;
  event_log2.events.push_back(make_event(addr, 0, 0));
  event_log2.events.push_back(make_event(addr, 0, 0));

  ASSERT_TRUE(db->consume_events(10, std::vector{event_log1}, {}));
  ASSERT_TRUE(db->consume_events(11, std::vector{event_log2}, {}));

  EventFilterList filters;
  filters.push_back(filter_with_contracts_no_topics({addr}));

  auto page = db->get_events(
      make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->events.size(), 3);
  EXPECT_EQ(page->events[0].block_height, 10);
  EXPECT_EQ(page->events[1].block_height, 11);
  EXPECT_EQ(page->events[1].sequence_number, 0);
  EXPECT_EQ(page->events[2].block_height, 11);
  EXPECT_EQ(page->events[2].sequence_number, 1);
  EXPECT_EQ(page->latest_block, 11);
  // Full exhaustion: cursor is empty.
  EXPECT_TRUE(page->cursor.empty());
}

TEST_F(IndexDBTest, GetEventsDescendingFromEnd)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  SHA256Hash tx1, tx2;
  std::ranges::fill(tx1, 0x01);
  std::ranges::fill(tx2, 0x02);

  TxEventLog event_log1;
  event_log1.proposed_tx_hash = tx1;
  event_log1.events.push_back(make_event(addr, 0, 0));

  TxEventLog event_log2;
  event_log2.proposed_tx_hash = tx2;
  event_log2.events.push_back(make_event(addr, 0, 0));

  ASSERT_TRUE(db->consume_events(10, std::vector{event_log1}, {}));
  ASSERT_TRUE(db->consume_events(11, std::vector{event_log2}, {}));

  EventFilterList filters;
  filters.push_back(filter_with_contracts_no_topics({addr}));

  auto page = db->get_events(
      make_query(std::move(filters), IterationOrder::IterationOrderDescending,
                 BlockAnchor(BlockAnchorKind::AnchorAtEnd)));
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->events.size(), 2);
  EXPECT_EQ(page->events[0].block_height, 11);
  EXPECT_EQ(page->events[1].block_height, 10);
}

TEST_F(IndexDBTest, GetEventsPaginationWithCursor)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);

  // Five blocks each with one matching event.
  for (int i = 0; i < 5; ++i) {
    SHA256Hash tx;
    std::ranges::fill(tx, 0x10 + i);
    TxEventLog log;
    log.proposed_tx_hash = tx;
    log.events.push_back(make_event(addr, 0, 0));
    ASSERT_TRUE(db->consume_events(10 + i, std::vector{log}, {}));
  }

  EventFilterList filters1;
  filters1.push_back(filter_with_contracts_no_topics({addr}));

  // First page: ascending from beginning, limit=2.
  auto page1 = db->get_events(
      make_query(std::move(filters1), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning),
                 /*limit=*/2));
  ASSERT_TRUE(page1.has_value());
  ASSERT_EQ(page1->events.size(), 2);
  EXPECT_EQ(page1->events[0].block_height, 10);
  EXPECT_EQ(page1->events[1].block_height, 11);
  ASSERT_FALSE(page1->cursor.empty());

  // Second page: same filter, cursor passed back.
  EventFilterList filters2;
  filters2.push_back(filter_with_contracts_no_topics({addr}));
  EventQuery q2 =
      make_query(std::move(filters2), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning),
                 /*limit=*/2);
  q2.cursor = page1->cursor;
  auto page2 = db->get_events(q2);
  ASSERT_TRUE(page2.has_value());
  ASSERT_EQ(page2->events.size(), 2);
  EXPECT_EQ(page2->events[0].block_height, 12);
  EXPECT_EQ(page2->events[1].block_height, 13);
  ASSERT_FALSE(page2->cursor.empty());

  // Third page exhausts.
  EventFilterList filters3;
  filters3.push_back(filter_with_contracts_no_topics({addr}));
  EventQuery q3 =
      make_query(std::move(filters3), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning),
                 /*limit=*/2);
  q3.cursor = page2->cursor;
  auto page3 = db->get_events(q3);
  ASSERT_TRUE(page3.has_value());
  ASSERT_EQ(page3->events.size(), 1);
  EXPECT_EQ(page3->events[0].block_height, 14);
  EXPECT_TRUE(page3->cursor.empty());
}

TEST_F(IndexDBTest, GetEventsTopicFilterPartialMatch)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  SHA256Hash tx;
  std::ranges::fill(tx, 0x01);

  // Two events on the same contract:
  //   ev[0]: num_topics=1, topic0 = 0x10 fill
  //   ev[1]: num_topics=3, topic0 = 0x20, topic1 = 0x21, topic2 = 0x22
  // Filter specifies topic0 == 0x20 only.  ev[1] should match (even
  // though it has extra topics); ev[0] should NOT (topic0 differs).
  TxEventLog log;
  log.proposed_tx_hash = tx;
  log.events.push_back(make_event(addr, 1, 0x10));
  log.events.push_back(make_event(addr, 3, 0x20));
  ASSERT_TRUE(db->consume_events(5, std::vector{log}, {}));

  EventFilter f;
  f.contract_addresses.push_back(addr);
  f.topics.topic0.push_back(topic_filled(0x20));
  EventFilterList filters;
  filters.push_back(f);

  auto page = db->get_events(
      make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->events.size(), 1);
  EXPECT_EQ(page->events[0].sequence_number, 1);
}

TEST_F(IndexDBTest, GetEventsOrAcrossFilters)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  // Wallet-style use case: "transfers to OR from me".  We fake it
  // with two filters that differ by topic1 vs topic2.
  ContractAddress token;
  std::ranges::fill(token, 0xAA);

  // Three events, all on `token`:
  //   ev[0]: topic0=0x10 (wrong), topic1=0x42 (me)
  //   ev[1]: topic0=0x10, topic1=0x42 (me), topic2=0x99
  //   ev[2]: topic0=0x10, topic1=0x99, topic2=0x42 (me)
  // With topic0 wrong in ev[0], it shouldn't match; ev[1] matches
  // "me in topic1"; ev[2] matches "me in topic2".
  //
  // But make_event fills all topics with a linear pattern so we
  // need a more careful constructor for this — inline below.
  auto make_event_manual = [&](uint32_t n,
                               std::initializer_list<uint8_t> topic_fills) {
    TxEmittedEvent e;
    e.contract_address = token;
    e.num_topics = n;
    e.serialized_event.resize(n * 32);
    size_t idx = 0;
    for (uint8_t f : topic_fills) {
      for (size_t i = 0; i < 32; ++i) {
        e.serialized_event[(idx * 32) + i] = f;
      }
      ++idx;
    }
    return e;
  };

  SHA256Hash tx;
  std::ranges::fill(tx, 0x01);
  TxEventLog log;
  log.proposed_tx_hash = tx;
  log.events.push_back(make_event_manual(2, {0x55, 0x42}));       // ev[0]
  log.events.push_back(make_event_manual(3, {0x10, 0x42, 0x99})); // ev[1]
  log.events.push_back(make_event_manual(3, {0x10, 0x99, 0x42})); // ev[2]
  ASSERT_TRUE(db->consume_events(5, std::vector{log}, {}));

  // Transfer-topic value 0x10, me 0x42.
  Topic transfer_topic = topic_filled(0x10);
  Topic me_topic = topic_filled(0x42);

  EventFilter from_me;
  from_me.contract_addresses.push_back(token);
  from_me.topics.topic0.push_back(transfer_topic);
  from_me.topics.topic1.push_back(me_topic);

  EventFilter to_me;
  to_me.contract_addresses.push_back(token);
  to_me.topics.topic0.push_back(transfer_topic);
  to_me.topics.topic2.push_back(me_topic);

  EventFilterList filters;
  filters.push_back(from_me);
  filters.push_back(to_me);

  auto page = db->get_events(
      make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->events.size(), 2);
  EXPECT_EQ(page->events[0].sequence_number, 1);
  EXPECT_EQ(page->events[1].sequence_number, 2);
}

TEST_F(IndexDBTest, GetEventsDedupContractAcrossFilters)
{
  // Two filters both include the same contract.  An event on that
  // contract that matches both filters' topics must be emitted
  // exactly once.
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  SHA256Hash tx;
  std::ranges::fill(tx, 0x01);

  TxEventLog log;
  log.proposed_tx_hash = tx;
  log.events.push_back(make_event(addr, 0, 0));
  ASSERT_TRUE(db->consume_events(1, std::vector{log}, {}));

  EventFilterList filters;
  filters.push_back(filter_with_contracts_no_topics({addr}));
  filters.push_back(filter_with_contracts_no_topics({addr}));

  auto page = db->get_events(
      make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->events.size(), 1);
}

TEST_F(IndexDBTest, GetEventsRejectsEmptyFilters)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  EventQuery q;
  q.order = IterationOrder::IterationOrderAscending;
  q.anchor = BlockAnchor(BlockAnchorKind::AnchorAtBeginning);
  q.limit = 10;
  // q.filters is empty.

  auto page = db->get_events(q);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->events.size(), 0);
  EXPECT_TRUE(page->cursor.empty());
}

TEST_F(IndexDBTest, GetEventsRejectsEmptyContractAddresses)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  EventFilterList filters;
  filters.push_back(EventFilter{}); // contract_addresses empty
  EventQuery q;
  q.filters = filters;
  q.order = IterationOrder::IterationOrderAscending;
  q.anchor = BlockAnchor(BlockAnchorKind::AnchorAtBeginning);
  q.limit = 10;

  auto page = db->get_events(q);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->events.size(), 0);
}

TEST_F(IndexDBTest, GetEventsAnchorAtHeightInclusive)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  for (int h : {5, 6, 7}) {
    SHA256Hash tx;
    std::ranges::fill(tx, static_cast<uint8_t>(h));
    TxEventLog log;
    log.proposed_tx_hash = tx;
    log.events.push_back(make_event(addr, 0, 0));
    ASSERT_TRUE(db->consume_events(h, std::vector{log}, {}));
  }

  EventFilterList filters;
  filters.push_back(filter_with_contracts_no_topics({addr}));
  auto page = db->get_events(make_query(std::move(filters),
                                        IterationOrder::IterationOrderAscending,
                                        anchor_at_height(6)));
  ASSERT_TRUE(page.has_value());
  // Anchor=6 inclusive, ascending -> blocks 6 and 7.
  ASSERT_EQ(page->events.size(), 2);
  EXPECT_EQ(page->events[0].block_height, 6);
  EXPECT_EQ(page->events[1].block_height, 7);
}

// Parity port of the legacy ConsumeAndLookupContractEvents test.
// Mixed-contract events within a single TxEventLog (the loop in
// consume_events indexes per-event, not per-tx); per-contract
// filtering returns the right counts and orderings; a contract
// with no events yields an empty page.
TEST_F(IndexDBTest, GetEventsMultiContractInOneTx)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  BlockHeight block_height = 200;
  SHA256Hash tx_hash1, tx_hash2;
  std::ranges::fill(tx_hash1, 0x10);
  std::ranges::fill(tx_hash2, 0x20);

  ContractAddress contract_addr1, contract_addr2;
  std::ranges::fill(contract_addr1, 0xCC);
  std::ranges::fill(contract_addr2, 0xDD);

  // First tx: two events, both from contract1.
  TxEventLog event_log1;
  event_log1.proposed_tx_hash = tx_hash1;
  event_log1.events.push_back(make_event(contract_addr1, 0, 0));
  event_log1.events.push_back(make_event(contract_addr1, 2, 0));

  // Second tx: one event from contract1 (seq 0), one from
  // contract2 (seq 1) — interleaved within the same TxEventLog.
  TxEventLog event_log2;
  event_log2.proposed_tx_hash = tx_hash2;
  event_log2.events.push_back(make_event(contract_addr1, 1, 0));
  event_log2.events.push_back(make_event(contract_addr2, 0, 0));

  ASSERT_TRUE(db->consume_events(block_height,
                                 std::vector{event_log1, event_log2}, {}));

  // Query contract1: expect 3 events (tx1 seq 0, tx1 seq 1, tx2 seq 0).
  {
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({contract_addr1}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                   BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->events.size(), 3);
    EXPECT_EQ(page->events[0].proposed_tx_hash, tx_hash1);
    EXPECT_EQ(page->events[0].sequence_number, 0);
    EXPECT_EQ(page->events[0].emitted_event.contract_address, contract_addr1);
    EXPECT_EQ(page->events[1].proposed_tx_hash, tx_hash1);
    EXPECT_EQ(page->events[1].sequence_number, 1);
    EXPECT_EQ(page->events[1].emitted_event.contract_address, contract_addr1);
    EXPECT_EQ(page->events[2].proposed_tx_hash, tx_hash2);
    EXPECT_EQ(page->events[2].sequence_number, 0);
    EXPECT_EQ(page->events[2].emitted_event.contract_address, contract_addr1);
  }

  // Query contract2: expect 1 event (tx2 seq 1, since the contract2
  // event was the second in event_log2's events vector).
  {
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({contract_addr2}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                   BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->events.size(), 1);
    EXPECT_EQ(page->events[0].proposed_tx_hash, tx_hash2);
    EXPECT_EQ(page->events[0].sequence_number, 1);
    EXPECT_EQ(page->events[0].emitted_event.contract_address, contract_addr2);
  }

  // Query a contract address with no events: expect an empty page.
  {
    ContractAddress contract_addr3;
    std::ranges::fill(contract_addr3, 0xEE);
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({contract_addr3}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                   BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
    ASSERT_TRUE(page.has_value());
    EXPECT_TRUE(page->events.empty());
    EXPECT_TRUE(page->cursor.empty());
  }
}

TEST_F(IndexDBTest, GetEventsLatestBlockTracking)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  // Initial: no ingestion, latest is 0.
  EXPECT_EQ(db->latest_block(), 0);

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  SHA256Hash tx;
  std::ranges::fill(tx, 0x01);
  TxEventLog log;
  log.proposed_tx_hash = tx;
  log.events.push_back(make_event(addr, 0, 0));

  ASSERT_TRUE(db->consume_events(42, std::vector{log}, {}));
  EXPECT_EQ(db->latest_block(), 42);

  // A height at or below the watermark is treated as already seen
  // and skipped; the watermark holds.
  SHA256Hash tx2;
  std::ranges::fill(tx2, 0x02);
  TxEventLog event_log2;
  event_log2.proposed_tx_hash = tx2;
  event_log2.events.push_back(make_event(addr, 0, 0));
  ASSERT_TRUE(db->consume_events(10, std::vector{event_log2}, {}));
  EXPECT_EQ(db->latest_block(), 42);
}

// Regression: with the SeekToLast() initializer, a database whose
// max-block-height entry lives under a contract that is NOT
// lexicographically last reported the wrong [latest_block] after
// restart.  The meta-key initializer must return the true max.
TEST_F(IndexDBTest, LatestBlockSurvivesRestartWithLowAddrHighBlock)
{
  // Low address, high block.
  ContractAddress addr_low;
  std::ranges::fill(addr_low, 0x01);
  SHA256Hash tx_low;
  std::ranges::fill(tx_low, 0x10);
  TxEventLog log_low;
  log_low.proposed_tx_hash = tx_low;
  log_low.events.push_back(make_event(addr_low, 0, 0));

  // High address, low block.
  ContractAddress addr_high;
  std::ranges::fill(addr_high, 0xFF);
  SHA256Hash tx_high;
  std::ranges::fill(tx_high, 0x20);
  TxEventLog log_high;
  log_high.proposed_tx_hash = tx_high;
  log_high.events.push_back(make_event(addr_high, 0, 0));

  {
    auto db = IndexDB::make_new(tmp_dir_.directory());
    ASSERT_TRUE(db.has_value());
    ASSERT_TRUE(db->consume_events(100, std::vector{log_high}, {}));
    ASSERT_TRUE(db->consume_events(101, std::vector{log_low}, {}));
    EXPECT_EQ(db->latest_block(), 101);
  }

  // Reopen.  SeekToLast on contract_index would land on addr_high
  // (block 100); the meta key keeps us at 101.
  auto db = IndexDB::open(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->latest_block(), 101);
}

// Databases that predate the meta key open cleanly: the missing key
// is treated as an empty database, latest_block defaults to 0, and
// the next consume_events writes the key.
TEST_F(IndexDBTest, LatestBlockRecoversAfterRestartWithoutMetaKey)
{
  {
    auto db = IndexDB::make_new(tmp_dir_.directory());
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->latest_block(), 0);
  }
  auto db = IndexDB::open(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->latest_block(), 0);
}

TEST_F(IndexDBTest, GetEventsInvalidCursorOrderReturnsEmpty)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  SHA256Hash tx1, tx2;
  std::ranges::fill(tx1, 0x01);
  std::ranges::fill(tx2, 0x02);

  TxEventLog event_log1;
  event_log1.proposed_tx_hash = tx1;
  event_log1.events.push_back(make_event(addr, 0, 0));

  TxEventLog event_log2;
  event_log2.proposed_tx_hash = tx2;
  event_log2.events.push_back(make_event(addr, 0, 0));

  ASSERT_TRUE(db->consume_events(5, std::vector{event_log1}, {}));
  ASSERT_TRUE(db->consume_events(10, std::vector{event_log2}, {}));

  EventCursorPayload payload;
  payload.schema_version = cursor::kCursorVersion;
  payload.order = static_cast<IterationOrder>(2);
  payload.block_height = 10;
  payload.tx_hash = tx2;
  payload.sequence_number = 0;
  std::string encoded = cursor::encode(payload);

  EventFilterList filters;
  filters.push_back(filter_with_contracts_no_topics({addr}));
  EventQuery query =
      make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                 BlockAnchor(BlockAnchorKind::AnchorAtBeginning));
  query.cursor.append_range(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size()));

  auto page = db->get_events(query);
  ASSERT_TRUE(page.has_value());
  EXPECT_TRUE(page->events.empty());
  EXPECT_TRUE(page->cursor.empty());
}

TEST_F(IndexDBTest, GetEventsCursorlessInvalidOrderReturnsEmpty)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  SHA256Hash tx1;
  std::ranges::fill(tx1, 0x01);

  TxEventLog event_log;
  event_log.proposed_tx_hash = tx1;
  event_log.events.push_back(make_event(addr, 0, 0));

  ASSERT_TRUE(db->consume_events(10, std::vector{event_log}, {}));

  EventFilterList filters;
  filters.push_back(filter_with_contracts_no_topics({addr}));
  auto page = db->get_events(make_query(std::move(filters),
                                        static_cast<IterationOrder>(2),
                                        anchor_at_height(10)));
  ASSERT_TRUE(page.has_value());
  EXPECT_TRUE(page->events.empty());
  EXPECT_TRUE(page->cursor.empty());
}

namespace {

// One-event TxEventLog for `addr` whose tx hash is uniformly `fill`.
TxEventLog
log_with_tx(ContractAddress const &addr, uint8_t fill)
{
  SHA256Hash tx;
  std::ranges::fill(tx, fill);
  TxEventLog log;
  log.proposed_tx_hash = tx;
  log.events.push_back(make_event(addr, 0, 0));
  return log;
}

} // namespace

TEST_F(IndexDBTest, OutOfOrderIngestHoldsWatermarkUntilGapFills)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);

  // First block defines the base — no need to start at 0.
  ASSERT_TRUE(db->consume_events(7, std::vector{log_with_tx(addr, 0x01)}, {}));
  EXPECT_EQ(db->latest_block(), 7);

  // Block 9 arrives before block 8: ingested but the watermark holds.
  ASSERT_TRUE(db->consume_events(9, std::vector{log_with_tx(addr, 0x03)}, {}));
  EXPECT_EQ(db->latest_block(), 7);

  // The gap fills; the watermark drains through the buffered height.
  ASSERT_TRUE(db->consume_events(8, std::vector{log_with_tx(addr, 0x02)}, {}));
  EXPECT_EQ(db->latest_block(), 9);
}

TEST_F(IndexDBTest, DuplicateIngestIsSkippedEntirely)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);

  ASSERT_TRUE(db->consume_events(7, std::vector{log_with_tx(addr, 0x01)}, {}));

  // Same height again with different content: ignored, success
  // returned, and the conflicting rows are NOT ingested (served
  // history is immutable).
  ASSERT_TRUE(db->consume_events(7, std::vector{log_with_tx(addr, 0x02)}, {}));
  EXPECT_EQ(db->latest_block(), 7);
  SHA256Hash tx2;
  std::ranges::fill(tx2, 0x02);
  EXPECT_FALSE(db->get_tx_events(tx2).has_value());

  // Below the base: also treated as already-seen.
  ASSERT_TRUE(db->consume_events(3, std::vector{log_with_tx(addr, 0x04)}, {}));
  EXPECT_EQ(db->latest_block(), 7);
  SHA256Hash tx4;
  std::ranges::fill(tx4, 0x04);
  EXPECT_FALSE(db->get_tx_events(tx4).has_value());
}

TEST_F(IndexDBTest, GetEventsDoesNotServeAboveContiguousWatermark)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);

  ASSERT_TRUE(db->consume_events(10, std::vector{log_with_tx(addr, 0x01)}, {}));
  // Out-of-order: block 12 lands before block 11.
  ASSERT_TRUE(db->consume_events(12, std::vector{log_with_tx(addr, 0x03)}, {}));
  ASSERT_EQ(db->latest_block(), 10);

  // Ascending full scan stops at the watermark: block 12's rows are
  // ingested but not servable, and the page reports exhaustion
  // (empty cursor) at a consistent cut.
  {
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({addr}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                   BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->events.size(), 1);
    EXPECT_EQ(page->events[0].block_height, 10);
    EXPECT_TRUE(page->cursor.empty());
    EXPECT_EQ(page->latest_block, 10);
  }

  // Descending from an explicit anchor above the watermark clamps
  // down to it instead of exposing the pending block.
  {
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({addr}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderDescending,
                   anchor_at_height(12)));
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->events.size(), 1);
    EXPECT_EQ(page->events[0].block_height, 10);
  }

  // Once the gap fills, everything becomes servable.
  ASSERT_TRUE(db->consume_events(11, std::vector{log_with_tx(addr, 0x02)}, {}));
  ASSERT_EQ(db->latest_block(), 12);
  {
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({addr}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                   BlockAnchor(BlockAnchorKind::AnchorAtBeginning)));
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->events.size(), 3);
    EXPECT_EQ(page->events[0].block_height, 10);
    EXPECT_EQ(page->events[1].block_height, 11);
    EXPECT_EQ(page->events[2].block_height, 12);
  }
}

TEST_F(IndexDBTest, GetEventsNegativeAnchorIsValidated)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);
  ASSERT_TRUE(db->consume_events(5, std::vector{log_with_tx(addr, 0x01)}, {}));

  // Descending from below the retained range: empty page — the
  // anchor must NOT wrap to the top of the key range and return the
  // newest event.
  {
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({addr}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderDescending,
                   anchor_at_height(-1)));
    ASSERT_TRUE(page.has_value());
    EXPECT_TRUE(page->events.empty());
    EXPECT_TRUE(page->cursor.empty());
  }

  // Ascending from below the retained range clamps up to
  // oldest_block() and serves normally.
  {
    EventFilterList filters;
    filters.push_back(filter_with_contracts_no_topics({addr}));
    auto page = db->get_events(
        make_query(std::move(filters), IterationOrder::IterationOrderAscending,
                   anchor_at_height(-1)));
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->events.size(), 1);
    EXPECT_EQ(page->events[0].block_height, 5);
  }
}

// tx_index is keyed by tx hash alone, so ingesting the same hash at a
// second height replaces the receipt while the first height's
// contract_index row stays durable.  Hydrating that stale row must not
// splice block 11's payload into a block 10 result.  The DB is opened
// inside the EXPECT_DEATH block so the fork happens before any RocksDB
// background threads exist.
TEST_F(IndexDBTest, GetEventsShadowedReceiptBlockMismatchIsFatal)
{
  // consume_events leaves its thread pool and RocksDB's background
  // threads alive, so the default fork-only death test style forks a
  // multi-threaded process.  "threadsafe" re-execs instead, which is
  // what keeps this from flaking under CI.
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  ContractAddress addr_c, addr_d;
  std::ranges::fill(addr_c, 0xC0);
  std::ranges::fill(addr_d, 0xD0);
  SHA256Hash tx;
  std::ranges::fill(tx, 0x42);

  {
    auto db = IndexDB::make_new(tmp_dir_.directory());
    ASSERT_TRUE(db.has_value());

    TxEventLog at_10;
    at_10.proposed_tx_hash = tx;
    at_10.events.push_back(make_event(addr_c, 0, 0));

    TxEventLog at_11;
    at_11.proposed_tx_hash = tx;
    at_11.events.push_back(make_event(addr_d, 0, 0));

    ASSERT_TRUE(db->consume_events(10, std::vector{at_10}, {}));
    ASSERT_TRUE(db->consume_events(11, std::vector{at_11}, {}));
  }

  const std::filesystem::path dir = tmp_dir_.directory();
  EXPECT_DEATH(
      {
        auto db = IndexDB::open(dir);
        ASSERT_TRUE(db.has_value());

        EventFilterList filters;
        filters.push_back(filter_with_contracts_no_topics({addr_c}));
        auto page =
            db->get_events(make_query(std::move(filters),
                                      IterationOrder::IterationOrderAscending,
                                      anchor_at_height(10)));
        (void)page;
      },
      "CHECK FAILED");
}

// NOLINTEND(misc-include-cleaner)

} // namespace hogd
