#include <algorithm>
#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <vector>

#include "event.hh"
#include "hogd/base/test.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

// basic tests on open/close db ops
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
  auto result = db->consume_events(block_height, events);
  ASSERT_TRUE(result.has_value());

  // Lookup first transaction
  auto lookup1 = db->get_tx_events(tx_hash1);
  ASSERT_TRUE(lookup1.has_value());
  EXPECT_EQ(lookup1->proposed_tx_hash, tx_hash1);
  EXPECT_EQ(lookup1->events.size(), 1);
  EXPECT_EQ(lookup1->events[0].contract_address, contract_addr1);

  // Lookup second transaction
  auto lookup2 = db->get_tx_events(tx_hash2);
  ASSERT_TRUE(lookup2.has_value());
  EXPECT_EQ(lookup2->proposed_tx_hash, tx_hash2);
  EXPECT_EQ(lookup2->events.size(), 1);
  EXPECT_EQ(lookup2->events[0].contract_address, contract_addr2);

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

// Test consuming events and looking them up by contract address
TEST_F(IndexDBTest, ConsumeAndLookupContractEvents)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());

  // Create test events with multiple contracts
  BlockHeight block_height = 200;
  SHA256Hash tx_hash1, tx_hash2;
  std::ranges::fill(tx_hash1, 0x10);
  std::ranges::fill(tx_hash2, 0x20);

  ContractAddress contract_addr1, contract_addr2;
  std::ranges::fill(contract_addr1, 0xCC);
  std::ranges::fill(contract_addr2, 0xDD);

  // First transaction has events from contract1
  TxEventLog event_log1;
  event_log1.proposed_tx_hash = tx_hash1;
  TxEmittedEvent event1a;
  event1a.contract_address = contract_addr1;
  event1a.num_topics = 0;
  event1a.serialized_event.resize(5);
  std::ranges::fill(event1a.serialized_event, 0x33);
  event_log1.events.push_back(event1a);

  TxEmittedEvent event1b;
  event1b.contract_address = contract_addr1;
  event1b.num_topics = 2;
  event1b.serialized_event.resize(15);
  std::ranges::fill(event1b.serialized_event, 0x44);
  event_log1.events.push_back(event1b);

  // Second transaction has events from contract1 and contract2
  TxEventLog event_log2;
  event_log2.proposed_tx_hash = tx_hash2;
  TxEmittedEvent event2a;
  event2a.contract_address = contract_addr1;
  event2a.num_topics = 1;
  event2a.serialized_event.resize(8);
  std::ranges::fill(event2a.serialized_event, 0x55);
  event_log2.events.push_back(event2a);

  TxEmittedEvent event2b;
  event2b.contract_address = contract_addr2;
  event2b.num_topics = 0;
  event2b.serialized_event.resize(12);
  std::ranges::fill(event2b.serialized_event, 0x66);
  event_log2.events.push_back(event2b);

  std::vector<TxEventLog> events = {event_log1, event_log2};

  // Consume events
  auto result = db->consume_events(block_height, events);
  ASSERT_TRUE(result.has_value());

  // Lookup events for contract1 (should have 3 events)
  auto contract1_events =
      db->get_contract_events(block_height, block_height + 1, contract_addr1);
  ASSERT_TRUE(contract1_events.has_value());
  EXPECT_EQ(contract1_events->size(), 3);

  // Verify the events are correct
  EXPECT_EQ((*contract1_events)[0].proposed_tx_hash, tx_hash1);
  EXPECT_EQ((*contract1_events)[0].sequence_number, 0);
  EXPECT_EQ((*contract1_events)[0].emitted_event.contract_address,
            contract_addr1);

  EXPECT_EQ((*contract1_events)[1].proposed_tx_hash, tx_hash1);
  EXPECT_EQ((*contract1_events)[1].sequence_number, 1);
  EXPECT_EQ((*contract1_events)[1].emitted_event.contract_address,
            contract_addr1);

  EXPECT_EQ((*contract1_events)[2].proposed_tx_hash, tx_hash2);
  EXPECT_EQ((*contract1_events)[2].sequence_number, 0);
  EXPECT_EQ((*contract1_events)[2].emitted_event.contract_address,
            contract_addr1);

  // Lookup events for contract2 (should have 1 event)
  auto contract2_events =
      db->get_contract_events(block_height, block_height + 1, contract_addr2);
  ASSERT_TRUE(contract2_events.has_value());
  EXPECT_EQ(contract2_events->size(), 1);
  EXPECT_EQ((*contract2_events)[0].proposed_tx_hash, tx_hash2);
  EXPECT_EQ((*contract2_events)[0].sequence_number, 1);
  EXPECT_EQ((*contract2_events)[0].emitted_event.contract_address,
            contract_addr2);

  // Lookup events for non-existent contract
  ContractAddress contract_addr3;
  std::ranges::fill(contract_addr3, 0xEE);
  auto contract3_events =
      db->get_contract_events(block_height, block_height + 1, contract_addr3);
  ASSERT_TRUE(contract3_events.has_value());
  EXPECT_TRUE(contract3_events->empty());
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
