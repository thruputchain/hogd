#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/recent_tx_failure_cache.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/rpc_from_event.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"
#include "indexer.hh"
#include "pagination.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class IndexerTest : public ::testing::Test {
public:
  IndexDB &db() { return *db_; }
  IndexerEventRpc &event() { return *event_; }
  IndexerBlockExplorerRpc &block_explorer() { return *block_explorer_; }
  RecentTxFailureCache &failure_cache() { return failure_cache_; }

  x23::Async<void>
  ingest_legacy_events(BlockHeight block_height,
                       const TxEventLogBatch &tx_events,
                       const ProposedTransactionBatch &tx_bodies)
  {
    CHECK(tx_events.size() == tx_bodies.size());
    CHECK(db_->consume_events(block_height, tx_events, tx_bodies).has_value());
    failure_cache_.prune(db_->latest_block());
    co_await Health::instance().set_current_block_height(block_height);
    co_return;
  }

protected:
  void SetUp() override
  {
    // Create a temporary test directory for the database.
    std::filesystem::create_directories(tmp_dir_.directory());
    std::filesystem::path db_path =
        std::filesystem::path(tmp_dir_.directory()) / "db";
    IndexerStatus<IndexDB> db_status = IndexDB::open_or_make_new(db_path);
    if (!db_status) {
      panic("[IndexDB::open_or_make_new] failed with {}", db_status.error());
    }
    {
      IndexDB &db = db_status.value();
      db_ = std::make_unique<IndexDB>(std::move(db));
    }
    CHECK(db_->is_open());

    event_ = std::make_unique<IndexerEventRpc>(*db_, failure_cache_);
    block_explorer_ =
        std::make_unique<IndexerBlockExplorerRpc>(*db_, failure_cache_);
  }

  TmpDir tmp_dir_;
  std::unique_ptr<IndexDB> db_;
  std::unique_ptr<IndexerEventRpc> event_;
  std::unique_ptr<IndexerBlockExplorerRpc> block_explorer_;
  RecentTxFailureCache failure_cache_;
};

namespace {

TxEventLogBatch
create_event_logs()
{
  // Create test events
  SHA256Hash tx_hash0, tx_hash1;
  std::ranges::fill(tx_hash0, 0x01);
  std::ranges::fill(tx_hash1, 0x02);

  ContractAddress contract_addr0, contract_addr1;
  std::ranges::fill(contract_addr0, 0xAA);
  std::ranges::fill(contract_addr1, 0xBB);

  TxEventLog event_log0;
  event_log0.proposed_tx_hash = tx_hash0;
  TxEmittedEvent event0;
  event0.contract_address = contract_addr0;
  event0.num_topics = 0;
  event0.serialized_event.resize(10);
  std::ranges::fill(event0.serialized_event, 0x11);
  event_log0.events.push_back(event0);

  TxEventLog event_log1;
  event_log1.proposed_tx_hash = tx_hash1;
  TxEmittedEvent event1;
  event1.contract_address = contract_addr1;
  event1.num_topics = 1;
  event1.serialized_event.resize(20);
  std::ranges::fill(event1.serialized_event, 0x22);
  event_log1.events.push_back(event1);

  return TxEventLogBatch{event_log0, event_log1};
}

// Bodies are forwarded index-aligned with the event logs.  Each body is given
// a distinct uniquifier nonce so tests can assert it round-trips through
// storage and the RPCs.
ProposedTransactionBatch
create_tx_bodies(std::size_t n)
{
  ProposedTransactionBatch bodies;
  for (std::size_t i = 0; i < n; i++) {
    ProposedTransaction body;
    body.signed_tx.tx.uniquifier.nonce = i;
    bodies.emplace_back(std::move(body));
  }
  return bodies;
}

TxFailureDiagnostic
make_failure_diag(BlockHeight attempt_height)
{
  TxFailureDiagnostic diag;
  diag.attempt_height = attempt_height;
  diag.conflict.code = DbConflictCode::kDbConflictValueMismatch;
  return diag;
}

} // namespace

TEST_F(IndexerTest, FindTx)
{
  BlockHeight block_height = 100;
  TxEventLogBatch events = create_event_logs();
  ProposedTransactionBatch bodies = create_tx_bodies(events.size());

  // Consume events.
  ingest_legacy_events(block_height, events, bodies)();

  // Look up the first tx.
  TxInfoResult result0 =
      block_explorer_->get_transaction_info(events[0].proposed_tx_hash)();
  EXPECT_EQ(result0.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result0.info().height, block_height);
  EXPECT_EQ(result0.info().tx, bodies[0]);
  EXPECT_EQ(result0.info().events.proposed_tx_hash, events[0].proposed_tx_hash);
  EXPECT_EQ(result0.info().events.events.size(), 1);
  EXPECT_EQ(result0.info().events.events[0].contract_address,
            events[0].events[0].contract_address);

  // Look up the second tx.
  TxInfoResult result1 =
      block_explorer_->get_transaction_info(events[1].proposed_tx_hash)();
  EXPECT_EQ(result1.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result1.info().height, block_height);
  EXPECT_EQ(result1.info().events.proposed_tx_hash, events[1].proposed_tx_hash);
  EXPECT_EQ(result1.info().events.events.size(), 1);
  EXPECT_EQ(result1.info().events.events[0].contract_address,
            events[1].events[0].contract_address);
}

TEST_F(IndexerTest, FindNonExistentTx)
{
  BlockHeight block_height = 100;
  TxEventLogBatch events = create_event_logs();
  ProposedTransactionBatch bodies = create_tx_bodies(events.size());

  // Consume events.
  ingest_legacy_events(block_height, events, bodies)();

  // Look up a non-existent tx.
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x03);
  TxInfoResult result = block_explorer_->get_transaction_info(tx_hash)();
  EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoNotFound);
}

// get_transactions returns each matched transaction with its full body, event
// log, and height -- i.e. the data the indexer now stores in the tx_index
// value (block_height || ProposedTransaction || TxEventLog).
TEST_F(IndexerTest, GetTransactionsReturnsBodiesAndEvents)
{
  BlockHeight block_height = 100;
  TxEventLogBatch events = create_event_logs(); // tx hashes 0x01, 0x02
  ProposedTransactionBatch bodies =
      create_tx_bodies(events.size()); // nonces 0, 1

  ingest_legacy_events(block_height, events, bodies)();

  TransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  query.limit = 0; // server default

  TransactionPage page = block_explorer_->get_transactions(query)();

  // Both txs are at block 100; ascending order is by (block, tx_hash), so
  // 0x01 (index 0) precedes 0x02 (index 1) -- aligned with events/bodies.
  ASSERT_EQ(page.transactions.size(), 2);
  for (std::size_t i = 0; i < page.transactions.size(); i++) {
    EXPECT_EQ(page.transactions[i].height, block_height);
    EXPECT_EQ(page.transactions[i].tx, bodies[i]);
    EXPECT_EQ(page.transactions[i].events.proposed_tx_hash,
              events[i].proposed_tx_hash);
    ASSERT_EQ(page.transactions[i].events.events.size(), 1);
    EXPECT_EQ(page.transactions[i].events.events[0].contract_address,
              events[i].events[0].contract_address);
  }
}

TEST_F(IndexerTest, GetTransactionsReturnsAllRegardlessOfAddress)
{
  BlockHeight block_height = 100;
  TxEventLogBatch events = create_event_logs(); // 2 txs, addrs 0xAA/0xBB
  ProposedTransactionBatch bodies = create_tx_bodies(events.size());
  ingest_legacy_events(block_height, events, bodies)();

  TransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  query.limit = 0;

  TransactionPage page = block_explorer_->get_transactions(query)();
  EXPECT_EQ(page.transactions.size(), 2);
}

TEST_F(IndexerTest, AddressQueryFindsEventlessRootInvocation)
{
  BlockHeight block_height = 101;

  ContractAddress root_addr;
  std::ranges::fill(root_addr, 0xCC);

  ProposedTransaction body;
  body.signed_tx.tx.uniquifier.nonce = 42;
  body.signed_tx.tx.invocation.invoked_address = root_addr;

  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x33);

  TxEventLog event_log; // no events emitted
  event_log.proposed_tx_hash = tx_hash;
  ASSERT_TRUE(event_log.events.empty());

  ingest_legacy_events(block_height, TxEventLogBatch{event_log},
                       ProposedTransactionBatch{body})();

  AddressTransactionQuery query{};
  query.address = root_addr;
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  query.limit = 0;

  TransactionRefPage page =
      block_explorer_->get_transactions_by_address(query)();
  ASSERT_EQ(page.refs.size(), 1);
  EXPECT_EQ(page.refs[0].tx_hash, tx_hash);
  EXPECT_EQ(page.refs[0].height, block_height);

  // The ref hydrates to the full body via the existing by-hash API.
  TxInfoResult info =
      block_explorer_->get_transaction_info(page.refs[0].tx_hash)();
  ASSERT_EQ(info.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(info.info().tx.signed_tx.tx.invocation.invoked_address, root_addr);
}

TEST_F(IndexerTest, AddressQuerySeparatesRootFromEmitter)
{
  BlockHeight block_height = 200;

  ContractAddress root_a, emitter_b;
  std::ranges::fill(root_a, 0xA1);
  std::ranges::fill(emitter_b, 0xB2);

  ProposedTransaction body;
  body.signed_tx.tx.invocation.invoked_address = root_a;
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x44);

  TxEventLog event_log;
  event_log.proposed_tx_hash = tx_hash;
  TxEmittedEvent ev;
  ev.contract_address = emitter_b; // event emitted by B, not the root A
  ev.num_topics = 0;
  ev.serialized_event.resize(4);
  event_log.events.push_back(ev);

  ingest_legacy_events(block_height, TxEventLogBatch{event_log},
                       ProposedTransactionBatch{body})();

  auto query_for = [&](ContractAddress const &addr) {
    AddressTransactionQuery q{};
    q.address = addr;
    q.order = IterationOrder::IterationOrderAscending;
    q.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
    q.limit = 0;
    return block_explorer_->get_transactions_by_address(q)();
  };

  // Rooted at A → in A's history.
  TransactionRefPage a_page = query_for(root_a);
  ASSERT_EQ(a_page.refs.size(), 1);
  EXPECT_EQ(a_page.refs[0].tx_hash, tx_hash);

  // B only emitted an event → NOT in B's transaction history.
  TransactionRefPage b_page = query_for(emitter_b);
  EXPECT_TRUE(b_page.refs.empty());

  // But B's event is still found via the event index.
  EventQuery eq{};
  EventFilter filter{};
  filter.contract_addresses.push_back(emitter_b);
  eq.filters.push_back(filter);
  eq.order = IterationOrder::IterationOrderAscending;
  eq.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  eq.limit = 0;
  EventPage ep = block_explorer_->get_events(eq)();
  EXPECT_EQ(ep.events.size(), 1);
}

TEST_F(IndexerTest, AddressQueryPaginatesInBlockOrder)
{
  ContractAddress addr;
  std::ranges::fill(addr, 0xD4);

  std::vector<SHA256Hash> hashes;
  for (uint8_t i = 0; i < 3; i++) {
    ProposedTransaction body;
    body.signed_tx.tx.uniquifier.nonce = i;
    body.signed_tx.tx.invocation.invoked_address = addr;
    SHA256Hash h;
    std::ranges::fill(h, static_cast<char>(0x50 + i));
    hashes.push_back(h);
    TxEventLog log;
    log.proposed_tx_hash = h;
    ingest_legacy_events(static_cast<BlockHeight>(300 + i),
                         TxEventLogBatch{log},
                         ProposedTransactionBatch{body})();
  }

  AddressTransactionQuery q{};
  q.address = addr;
  q.order = IterationOrder::IterationOrderAscending;
  q.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  q.limit = 1;

  std::vector<SHA256Hash> seen;
  for (int guard = 0; guard < 10; guard++) {
    TransactionRefPage page = block_explorer_->get_transactions_by_address(q)();
    for (auto const &r : page.refs) {
      seen.push_back(r.tx_hash);
    }
    if (page.cursor.empty()) {
      break;
    }
    q.cursor = page.cursor;
  }

  ASSERT_EQ(seen.size(), 3);
  EXPECT_EQ(seen[0], hashes[0]); // block 300
  EXPECT_EQ(seen[1], hashes[1]); // block 301
  EXPECT_EQ(seen[2], hashes[2]); // block 302
}

TEST_F(IndexerTest, PaginationFreezesRowsButReportsCurrentWatermark)
{
  ContractAddress addr;
  std::ranges::fill(addr, 0xD5);

  auto ingest = [&](BlockHeight height, uint8_t fill) {
    ProposedTransaction body;
    body.signed_tx.tx.uniquifier.nonce = fill;
    body.signed_tx.tx.invocation.invoked_address = addr;
    TxEventLog log;
    std::ranges::fill(log.proposed_tx_hash, fill);
    TxEmittedEvent event;
    event.contract_address = addr;
    log.events.push_back(event);
    ingest_legacy_events(height, TxEventLogBatch{log},
                         ProposedTransactionBatch{body})();
  };

  ingest(300, 0x60);
  ingest(301, 0x61);

  EventQuery event_query{};
  event_query.filters.push_back(
      EventFilter{.contract_addresses = {addr}, .topics = {}});
  event_query.order = IterationOrder::IterationOrderAscending;
  event_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  event_query.limit = 1;
  EventPage event_page = block_explorer_->get_events(event_query)();
  ASSERT_EQ(event_page.events.size(), 1UL);
  ASSERT_FALSE(event_page.cursor.empty());

  TransactionQuery transaction_query{};
  transaction_query.order = IterationOrder::IterationOrderAscending;
  transaction_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  transaction_query.limit = 1;
  TransactionPage transaction_page =
      block_explorer_->get_transactions(transaction_query)();
  ASSERT_EQ(transaction_page.transactions.size(), 1UL);
  ASSERT_FALSE(transaction_page.cursor.empty());

  AddressTransactionQuery address_query{};
  address_query.address = addr;
  address_query.order = IterationOrder::IterationOrderAscending;
  address_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  address_query.limit = 1;
  TransactionRefPage address_page =
      block_explorer_->get_transactions_by_address(address_query)();
  ASSERT_EQ(address_page.refs.size(), 1UL);
  ASSERT_FALSE(address_page.cursor.empty());

  ingest(302, 0x62);

  event_query.cursor = event_page.cursor;
  event_page = block_explorer_->get_events(event_query)();
  ASSERT_EQ(event_page.events.size(), 1UL);
  EXPECT_EQ(event_page.events[0].block_height, 301);
  EXPECT_EQ(event_page.latest_block, 302);
  EXPECT_TRUE(event_page.cursor.empty());

  transaction_query.cursor = transaction_page.cursor;
  transaction_page = block_explorer_->get_transactions(transaction_query)();
  ASSERT_EQ(transaction_page.transactions.size(), 1UL);
  EXPECT_EQ(transaction_page.transactions[0].height, 301);
  EXPECT_EQ(transaction_page.latest_block, 302);
  EXPECT_TRUE(transaction_page.cursor.empty());

  address_query.cursor = address_page.cursor;
  address_page = block_explorer_->get_transactions_by_address(address_query)();
  ASSERT_EQ(address_page.refs.size(), 1UL);
  EXPECT_EQ(address_page.refs[0].height, 301);
  EXPECT_EQ(address_page.latest_block, 302);
  EXPECT_TRUE(address_page.cursor.empty());
}

// Regression: a batch in which NO transaction emits any event (every
// TxEventLog is present but has an empty events vector) must still write a
// ROOT_TX row for every body.  Guards against the contract-index chunking
// being driven by emitted-event count rather than transaction count.
TEST_F(IndexerTest, EventlessBatchStillGetsRootTxEntries)
{
  BlockHeight block_height = 500;

  std::vector<ContractAddress> addrs;
  TxEventLogBatch logs;
  ProposedTransactionBatch bodies;
  for (uint8_t i = 0; i < 4; i++) {
    ContractAddress addr;
    std::ranges::fill(addr, static_cast<char>(0x60 + i));
    addrs.push_back(addr);

    SHA256Hash h;
    std::ranges::fill(h, static_cast<char>(0x90 + i));

    TxEventLog log;
    log.proposed_tx_hash = h;
    logs.push_back(log); // present, but emits no events

    ProposedTransaction body;
    body.signed_tx.tx.uniquifier.nonce = i;
    body.signed_tx.tx.invocation.invoked_address = addr;
    bodies.push_back(body);
  }

  // Whole batch ingested in a single call: events.size() == 4, but the total
  // number of emitted events across the batch is 0.
  ingest_legacy_events(block_height, logs, bodies)();

  for (uint8_t i = 0; i < 4; i++) {
    AddressTransactionQuery q{};
    q.address = addrs[i];
    q.order = IterationOrder::IterationOrderAscending;
    q.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
    q.limit = 0;

    // Each eventless tx must still have produced its ROOT_TX row.
    TransactionRefPage page = block_explorer_->get_transactions_by_address(q)();
    ASSERT_EQ(page.refs.size(), 1);
    EXPECT_EQ(page.refs[0].tx_hash, logs[i].proposed_tx_hash);
  }
}

TEST_F(IndexerTest, RecentFailureVisibleThenCommittedRecordWins)
{
  TxEventLogBatch events = create_event_logs();
  ProposedTransactionBatch bodies = create_tx_bodies(events.size());
  const SignedTransactionHash tx_hash = events[0].proposed_tx_hash;

  // Report a failure for a tx with no committed record.
  event_->indexer_event_report_tx_failure(tx_hash, make_failure_diag(100))();

  TxInfoResult result = block_explorer_->get_transaction_info(tx_hash)();
  ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoRecentFailure);
  EXPECT_EQ(result.recent_failure().attempt_height, 100);
  EXPECT_EQ(result.recent_failure().conflict.code,
            DbConflictCode::kDbConflictValueMismatch);

  // The retried tx commits at height 101: the committed record wins.
  ingest_legacy_events(101, events, bodies)();
  result = block_explorer_->get_transaction_info(tx_hash)();
  ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result.info().height, 101);
}

TEST_F(IndexerTest, RecentFailureExpiresAsWatermarkAdvances)
{
  SignedTransactionHash failed_hash;
  std::ranges::fill(failed_hash, 0x7F);
  event_->indexer_event_report_tx_failure(failed_hash,
                                          make_failure_diag(100))();

  // Ingest committed blocks 100..105; each ingest prunes against the new
  // watermark. Give each block one distinct committed tx.
  for (BlockHeight h = 100; h <= 105; h++) {
    TxEventLog log;
    std::ranges::fill(log.proposed_tx_hash, static_cast<uint8_t>(h));
    ProposedTransaction body;
    body.signed_tx.tx.uniquifier.nonce = h;
    ingest_legacy_events(h, TxEventLogBatch{log},
                         ProposedTransactionBatch{body})();
    TxInfoResult result = block_explorer_->get_transaction_info(failed_hash)();
    if (h < 105) {
      // attempt 100 + delta 5 > watermark h -> still visible.
      EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoRecentFailure)
          << "watermark " << h;
    }
    else {
      EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoNotFound);
    }
  }
}

TEST_F(IndexerTest, UnknownTxStillNotFound)
{
  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x03);
  TxInfoResult result = block_explorer_->get_transaction_info(tx_hash)();
  EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoNotFound);
}

// Legacy ingest can still create a pending row above its watermark. All public
// read surfaces, including point lookup, must hide it until the gap closes.
TEST_F(IndexerTest, PendingTxHiddenFromPointLookupAndScans)
{
  // One address is both the event emitter and the root invoked_address, so it
  // is the single discriminator for the event index, the tx list, and the
  // address index alike.
  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);

  SHA256Hash tx10, tx12;
  std::ranges::fill(tx10, 0x10);
  std::ranges::fill(tx12, 0x12);

  auto ingest_at = [&](BlockHeight height, SHA256Hash const &tx) {
    TxEventLog log;
    log.proposed_tx_hash = tx;
    TxEmittedEvent ev;
    ev.contract_address = addr;
    ev.num_topics = 0;
    ev.serialized_event.resize(4);
    log.events.push_back(std::move(ev));

    ProposedTransaction body;
    body.signed_tx.tx.invocation.invoked_address = addr;

    ingest_legacy_events(height, TxEventLogBatch{log},
                         ProposedTransactionBatch{body})();
  };

  // Block 10 is contiguous; block 12 arrives before block 11, so it is
  // ingested but held behind the watermark at 10.
  ingest_at(10, tx10);
  ingest_at(12, tx12);
  ASSERT_EQ(db().latest_block(), 10);

  // Point lookup cannot expose a row above the durable serving boundary.
  TxInfoResult pending = block_explorer_->get_transaction_info(tx12)();
  ASSERT_EQ(pending.outcome(), TxInfoOutcome::TxInfoNotFound);

  // The block-10 tx below the watermark is servable too (positive control).
  TxInfoResult settled = block_explorer_->get_transaction_info(tx10)();
  ASSERT_EQ(settled.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(settled.info().height, 10);

  // Scan 1 -- events by address: only the block-10 event is served.
  EventQuery event_query{};
  EventFilter filter{};
  filter.contract_addresses.push_back(addr);
  event_query.filters.push_back(std::move(filter));
  event_query.order = IterationOrder::IterationOrderAscending;
  event_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  event_query.limit = 0;

  EventPage event_page = block_explorer_->get_events(event_query)();
  EXPECT_EQ(event_page.latest_block, 10);
  ASSERT_EQ(event_page.events.size(), 1);
  EXPECT_EQ(event_page.events[0].block_height, 10);
  EXPECT_EQ(event_page.events[0].proposed_tx_hash, tx10);

  // Scan 2 -- unfiltered transaction list: only block 10 appears.
  TransactionQuery tx_query{};
  tx_query.order = IterationOrder::IterationOrderAscending;
  tx_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  tx_query.limit = 0;

  TransactionPage tx_page = block_explorer_->get_transactions(tx_query)();
  EXPECT_EQ(tx_page.latest_block, 10);
  ASSERT_EQ(tx_page.transactions.size(), 1);
  EXPECT_EQ(tx_page.transactions[0].height, 10);

  // Scan 3 -- indexed address query (get_transactions_by_address): the pending
  // block-12 ref is hidden even though its tx is point-lookup visible.
  AddressTransactionQuery addr_query{};
  addr_query.address = addr;
  addr_query.order = IterationOrder::IterationOrderAscending;
  addr_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  addr_query.limit = 0;

  TransactionRefPage addr_page =
      block_explorer_->get_transactions_by_address(addr_query)();
  EXPECT_EQ(addr_page.latest_block, 10);
  ASSERT_EQ(addr_page.refs.size(), 1);
  EXPECT_EQ(addr_page.refs[0].tx_hash, tx10);
  EXPECT_EQ(addr_page.refs[0].height, 10);
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
