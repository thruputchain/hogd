#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "event.hh"
#include "event_cursor_internal.hh"
#include "hogd/base/log.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_indexer/db.h"
#include "hogd/hog_indexer/event_cursor.h"
#include "hogd/hog_indexer/recent_tx_failure_cache.h"
#include "hogd/hog_indexer/rpc_from_block_explorer.h"
#include "hogd/hog_indexer/status.h"
#include "hogd/init.h"
#include "hogd/metrics/health.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "hogd/xdr/protocol_token.h"
#include "indexer.hh"
#include "pagination.hh"
#include "proposal_hash.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"

namespace hogd {

class IndexerTest : public ::testing::Test {
public:
  IndexDB &db() { return *db_; }
  IndexerBlockExplorerRpc &block_explorer() { return *block_explorer_; }
  RecentTxFailureCache &failure_cache() { return failure_cache_; }

  x23::Async<void>
  ingest_legacy_events(BlockHeight block_height,
                       const TxEventLogBatch &tx_events,
                       const ProposedTransactionBatch &tx_bodies)
  {
    CHECK(tx_events.size() == tx_bodies.size());
    CHECK(db_->consume_events(block_height, tx_events, tx_bodies).has_value());
    failure_cache_.prune(*db_->latest_block());
    co_await Health::instance().set_current_block_height(block_height);
    co_return;
  }

protected:
  // Address history requires contiguous committed evidence, including empty
  // blocks before the fixture's requested height.
  x23::Async<void>
  ingest_committed_events(BlockHeight height, const TxEventLogBatch &logs,
                          const ProposedTransactionBatch &bodies,
                          IndexDB *database = nullptr)
  {
    auto &index = database ? *database : *db_;
    auto progress = index.durable_progress();
    if (progress.state() ==
        IndexerProgressState::IndexerProgressUninitialized) {
      IndexerOutboxOrigin origin{};
      origin.protocol = protocol_from_text("hogp/test");
      origin.first_height = 1;
      origin.first_prev_hash[0] = 1;
      CHECK_EQ(index.initialize_epoch(origin).outcome(),
               IndexerEpochOutcome::IndexerEpochInitialized);
      progress = index.durable_progress();
    }
    auto current = progress.initialized();
    for (BlockHeight next = current.contiguous_height + 1; next <= height;
         ++next) {
      IndexerCommittedBlock block{};
      auto &payload = block.payload;
      payload.format_version = kIndexerOutboxFormatVersion;
      if (next == height) {
        payload.event_logs = logs;
        payload.tx_bodies = bodies;
      }
      auto &value = payload.consensus_value;
      value.proposal_header.block_height = next;
      value.prev_hash = current.last_consensus_hash;
      auto roots =
          compute_indexer_payload_roots(payload.event_logs, payload.tx_bodies);
      CHECK(roots.has_value());
      value.proposal_header.events_hash = roots->events_hash;
      value.proposal_header.txs_hash = roots->txs_hash;
      ProposalTxs txs;
      SignedTransactionHashBatch signed_hashes;
      x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
      for (const auto &body : payload.tx_bodies) {
        signed_hashes.push_back(sha256(body.signed_tx));
        nondeterministic_data.push_back(body.nondeterministic_proposal_data);
      }
      txs.signed_hash = t_to_parallel_batch(signed_hashes)();
      txs.nondeterministic_data = t_to_parallel_batch(nondeterministic_data)();
      value.proposal_txs_hash = proposal_txs_compatibility_hash(txs);
      block.identity.consensus_hash = sha256(value);
      CHECK_EQ((co_await index.apply_committed_block(current.origin, block))
                   .outcome(),
               IndexerBlockApplyOutcome::IndexerBlockApplied);
      current = index.durable_progress().initialized();
    }
    co_return;
  }

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

    block_explorer_ =
        std::make_unique<IndexerBlockExplorerRpc>(*db_, failure_cache_);
  }

  TmpDir tmp_dir_;
  std::unique_ptr<IndexDB> db_;
  std::unique_ptr<IndexerBlockExplorerRpc> block_explorer_;
  RecentTxFailureCache failure_cache_;
};

namespace {

TxEventLogBatch
create_event_logs()
{
  // Create test events
  ProposedTransaction first{}, second{};
  second.signed_tx.tx.uniquifier.nonce = 1;
  const auto tx_hash0 = sha256(first.signed_tx.tx);
  const auto tx_hash1 = sha256(second.signed_tx.tx);

  ContractAddress contract_addr0, contract_addr1;
  std::ranges::fill(contract_addr0, 0xAA);
  std::ranges::fill(contract_addr1, 0xBB);

  TxEventLog event_log0{};
  event_log0.tx_hash = tx_hash0;
  TxEmittedEvent event0;
  event0.contract_address = contract_addr0;
  event0.num_topics = 0;
  event0.serialized_event.resize(10);
  std::ranges::fill(event0.serialized_event, 0x11);
  event_log0.events.push_back(event0);

  TxEventLog event_log1{};
  event_log1.tx_hash = tx_hash1;
  TxEmittedEvent event1;
  event1.contract_address = contract_addr1;
  event1.num_topics = 1;
  event1.serialized_event.resize(32);
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
    ProposedTransaction body{};
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
  diag.cause.conflict().code = DbConflictCode::ValueMismatch;
  return diag;
}

// Unwraps a page-query result, recording a test failure (and returning an
// empty page) when the backend reported an error instead of a page.
template<typename Result>
auto
unwrap_page(Result result)
{
  using Page = std::remove_cvref_t<decltype(result.page())>;
  if (result.outcome() != IndexerQueryOutcome::IndexerQueryOk) {
    ADD_FAILURE() << "page query failed: outcome "
                  << static_cast<int>(result.outcome());
    return Page{};
  }
  return std::move(result.page());
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
      block_explorer_->get_transaction_info(events[0].tx_hash)();
  EXPECT_EQ(result0.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result0.info().height, block_height);
  EXPECT_EQ(result0.info().tx, bodies[0]);
  EXPECT_EQ(result0.info().events.tx_hash, events[0].tx_hash);
  EXPECT_EQ(result0.info().events.events.size(), 1);
  EXPECT_EQ(result0.info().events.events[0].contract_address,
            events[0].events[0].contract_address);

  // Look up the second tx.
  TxInfoResult result1 =
      block_explorer_->get_transaction_info(events[1].tx_hash)();
  EXPECT_EQ(result1.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result1.info().height, block_height);
  EXPECT_EQ(result1.info().events.tx_hash, events[1].tx_hash);
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
  TxEventLogBatch events = create_event_logs();
  ProposedTransactionBatch bodies =
      create_tx_bodies(events.size()); // nonces 0, 1

  ingest_legacy_events(block_height, events, bodies)();

  TransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  query.limit = 0; // server default

  TransactionPage page =
      unwrap_page(block_explorer_->get_transactions(query)());

  // Both txs are at block 100; ascending order is by transaction hash.
  const std::array<size_t, 2> indices = events[0].tx_hash < events[1].tx_hash
                                            ? std::array<size_t, 2>{0, 1}
                                            : std::array<size_t, 2>{1, 0};
  ASSERT_EQ(page.transactions.size(), 2);
  for (std::size_t i = 0; i < page.transactions.size(); i++) {
    const auto source = indices[i];
    EXPECT_EQ(page.transactions[i].height, block_height);
    EXPECT_EQ(page.transactions[i].tx, bodies[source]);
    EXPECT_EQ(page.transactions[i].events.tx_hash, events[source].tx_hash);
    ASSERT_EQ(page.transactions[i].events.events.size(), 1);
    EXPECT_EQ(page.transactions[i].events.events[0].contract_address,
              events[source].events[0].contract_address);
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

  TransactionPage page =
      unwrap_page(block_explorer_->get_transactions(query)());
  EXPECT_EQ(page.transactions.size(), 2);
}

TEST_F(IndexerTest, AddressQueryFindsEventlessRootInvocation)
{
  BlockHeight block_height = 101;

  ContractAddress root_addr;
  std::ranges::fill(root_addr, 0xCC);

  ProposedTransaction body{};
  body.signed_tx.tx.uniquifier.nonce = 42;
  body.signed_tx.tx.invocation.invoked_address = root_addr;

  const auto tx_hash = sha256(body.signed_tx.tx);

  TxEventLog event_log{}; // no events emitted
  event_log.tx_hash = tx_hash;
  ASSERT_TRUE(event_log.events.empty());

  ingest_committed_events(block_height, TxEventLogBatch{event_log},
                          ProposedTransactionBatch{body})();

  AddressTransactionQuery query{};
  query.address = root_addr;
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  query.limit = 0;

  AddressTransactionRefPage page =
      unwrap_page(block_explorer_->get_transactions_by_address(query)());
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

  ProposedTransaction body{};
  body.signed_tx.tx.invocation.invoked_address = root_a;
  const auto tx_hash = sha256(body.signed_tx.tx);

  TxEventLog event_log{};
  event_log.tx_hash = tx_hash;
  TxEmittedEvent ev;
  ev.contract_address = emitter_b; // event emitted by B, not the root A
  ev.num_topics = 0;
  ev.serialized_event.resize(4);
  event_log.events.push_back(ev);

  ingest_committed_events(block_height, TxEventLogBatch{event_log},
                          ProposedTransactionBatch{body})();

  auto query_for = [&](ContractAddress const &addr) {
    AddressTransactionQuery q{};
    q.address = addr;
    q.order = IterationOrder::IterationOrderAscending;
    q.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
    q.limit = 0;
    return unwrap_page(block_explorer_->get_transactions_by_address(q)());
  };

  // Rooted at A → in A's history.
  AddressTransactionRefPage a_page = query_for(root_a);
  ASSERT_EQ(a_page.refs.size(), 1);
  EXPECT_EQ(a_page.refs[0].tx_hash, tx_hash);

  // B only emitted an event → NOT in B's transaction history.
  AddressTransactionRefPage b_page = query_for(emitter_b);
  EXPECT_TRUE(b_page.refs.empty());

  // But B's event is still found via the event index.
  EventQuery eq{};
  EventFilter filter{};
  filter.contract_addresses.push_back(emitter_b);
  eq.filters.push_back(filter);
  eq.order = IterationOrder::IterationOrderAscending;
  eq.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  eq.limit = 0;
  EventPage ep = unwrap_page(block_explorer_->get_events(eq)());
  EXPECT_EQ(ep.events.size(), 1);
}

TEST_F(IndexerTest, AddressQueryPaginatesInBlockOrder)
{
  ContractAddress addr;
  std::ranges::fill(addr, 0xD4);

  std::vector<SHA256Hash> hashes;
  for (uint8_t i = 0; i < 3; i++) {
    ProposedTransaction body{};
    body.signed_tx.tx.uniquifier.nonce = i;
    body.signed_tx.tx.invocation.invoked_address = addr;
    const auto h = sha256(body.signed_tx.tx);
    hashes.push_back(h);
    TxEventLog log{};
    log.tx_hash = h;
    ingest_committed_events(static_cast<BlockHeight>(300 + i),
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
    AddressTransactionRefPage page =
        unwrap_page(block_explorer_->get_transactions_by_address(q)());
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
    ProposedTransaction body{};
    body.signed_tx.tx.uniquifier.nonce = fill;
    body.signed_tx.tx.invocation.invoked_address = addr;
    TxEventLog log{};
    log.tx_hash = sha256(body.signed_tx.tx);
    TxEmittedEvent event{};
    event.contract_address = addr;
    log.events.push_back(event);
    ingest_committed_events(height, TxEventLogBatch{log},
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
  EventPage event_page =
      unwrap_page(block_explorer_->get_events(event_query)());
  ASSERT_EQ(event_page.events.size(), 1UL);
  ASSERT_FALSE(event_page.cursor.empty());

  TransactionQuery transaction_query{};
  transaction_query.order = IterationOrder::IterationOrderAscending;
  transaction_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  transaction_query.limit = 1;
  TransactionPage transaction_page =
      unwrap_page(block_explorer_->get_transactions(transaction_query)());
  ASSERT_EQ(transaction_page.transactions.size(), 1UL);
  ASSERT_FALSE(transaction_page.cursor.empty());

  AddressTransactionQuery address_query{};
  address_query.address = addr;
  address_query.order = IterationOrder::IterationOrderAscending;
  address_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  address_query.limit = 1;
  AddressTransactionRefPage address_page = unwrap_page(
      block_explorer_->get_transactions_by_address(address_query)());
  ASSERT_EQ(address_page.refs.size(), 1UL);
  ASSERT_FALSE(address_page.cursor.empty());

  ingest(302, 0x62);

  event_query.cursor = event_page.cursor;
  event_page = unwrap_page(block_explorer_->get_events(event_query)());
  ASSERT_EQ(event_page.events.size(), 1UL);
  EXPECT_EQ(event_page.events[0].block_height, 301);
  EXPECT_EQ(event_page.latest_block, 302);
  EXPECT_TRUE(event_page.cursor.empty());

  transaction_query.cursor = transaction_page.cursor;
  transaction_page =
      unwrap_page(block_explorer_->get_transactions(transaction_query)());
  ASSERT_EQ(transaction_page.transactions.size(), 1UL);
  EXPECT_EQ(transaction_page.transactions[0].height, 301);
  EXPECT_EQ(transaction_page.latest_block, 302);
  EXPECT_TRUE(transaction_page.cursor.empty());

  address_query.cursor = address_page.cursor;
  address_page = unwrap_page(
      block_explorer_->get_transactions_by_address(address_query)());
  ASSERT_EQ(address_page.refs.size(), 1UL);
  EXPECT_EQ(address_page.refs[0].height, 301);
  EXPECT_EQ(address_page.latest_block, 301);
  EXPECT_TRUE(address_page.cursor.empty());
}

TEST_F(IndexerTest, HistoryCursorsBindMethodAndRootAddress)
{
  ContractAddress root{};
  std::ranges::fill(root, 0xC1);
  for (uint8_t i = 0; i < 4; ++i) {
    ProposedTransaction body{};
    body.signed_tx.tx.invocation.invoked_address = root;
    body.signed_tx.tx.uniquifier.nonce = i;
    TxEventLog log{};
    log.tx_hash = sha256(body.signed_tx.tx);
    TxEmittedEvent event{};
    event.contract_address = root;
    log.events.push_back(event);
    ingest_committed_events(10 + i, TxEventLogBatch{log},
                            ProposedTransactionBatch{body})();
  }
  for (const auto order : {IterationOrder::IterationOrderAscending,
                           IterationOrder::IterationOrderDescending}) {
    SCOPED_TRACE(static_cast<int>(order));
    const bool ascending = order == IterationOrder::IterationOrderAscending;
    EventQuery events{};
    events.filters.push_back(
        EventFilter{.contract_addresses = {root}, .topics = {}});
    events.order = order;
    events.anchor.kind(ascending ? BlockAnchorKind::AnchorAtBeginning
                                 : BlockAnchorKind::AnchorAtEnd);
    events.limit = 1;
    TransactionQuery transactions{};
    transactions.order = order;
    transactions.anchor = events.anchor;
    transactions.limit = 1;
    AddressTransactionQuery address{};
    address.address = root;
    address.order = order;
    address.anchor = events.anchor;
    address.limit = 1;
    auto event_page = unwrap_page(block_explorer_->get_events(events)());
    auto tx_page =
        unwrap_page(block_explorer_->get_transactions(transactions)());
    auto address_page =
        unwrap_page(block_explorer_->get_transactions_by_address(address)());
    ASSERT_FALSE(event_page.cursor.empty());
    ASSERT_FALSE(tx_page.cursor.empty());
    ASSERT_FALSE(address_page.cursor.empty());
    const std::vector<PaginationCursor> cursors{event_page.cursor,
                                                tx_page.cursor,
                                                address_page.cursor};
    for (size_t i = 0; i < cursors.size(); ++i) {
      SCOPED_TRACE(i);
      events.cursor = cursors[i];
      transactions.cursor = cursors[i];
      address.cursor = cursors[i];
      EXPECT_EQ(block_explorer_->get_events(events)().outcome(),
                i == 0 ? IndexerQueryOutcome::IndexerQueryOk
                       : IndexerQueryOutcome::IndexerQueryInvalidCursor);
      EXPECT_EQ(block_explorer_->get_transactions(transactions)().outcome(),
                i == 1 ? IndexerQueryOutcome::IndexerQueryOk
                       : IndexerQueryOutcome::IndexerQueryInvalidCursor);
      EXPECT_EQ(
          block_explorer_->get_transactions_by_address(address)().outcome(),
          i == 2 ? IndexerQueryOutcome::IndexerQueryOk
                 : IndexerQueryOutcome::IndexerQueryInvalidCursor);
    }
    address.cursor = address_page.cursor;
    address.address[0] ^= 0xFF;
    EXPECT_EQ(block_explorer_->get_transactions_by_address(address)().outcome(),
              IndexerQueryOutcome::IndexerQueryInvalidCursor);
    address.address = root;

    // Continuation owns the direction and anchor, but page size can change.
    events.cursor = event_page.cursor;
    transactions.cursor = tx_page.cursor;
    events.order = transactions.order = address.order =
        ascending ? IterationOrder::IterationOrderDescending
                  : IterationOrder::IterationOrderAscending;
    events.anchor.kind(ascending ? BlockAnchorKind::AnchorAtEnd
                                 : BlockAnchorKind::AnchorAtBeginning);
    transactions.anchor = address.anchor = events.anchor;
    events.limit = transactions.limit = address.limit = 2;
    event_page = unwrap_page(block_explorer_->get_events(events)());
    tx_page = unwrap_page(block_explorer_->get_transactions(transactions)());
    address_page =
        unwrap_page(block_explorer_->get_transactions_by_address(address)());
    ASSERT_EQ(event_page.events.size(), 2);
    ASSERT_EQ(tx_page.transactions.size(), 2);
    ASSERT_EQ(address_page.refs.size(), 2);
    for (size_t i = 0; i < 2; ++i) {
      const BlockHeight height = ascending ? 11 + i : 12 - i;
      EXPECT_EQ(event_page.events[i].block_height, height);
      EXPECT_EQ(tx_page.transactions[i].height, height);
      EXPECT_EQ(address_page.refs[i].height, height);
    }
  }
}

TEST_F(IndexerTest, MalformedHistoryCursorsHaveTypedOutcomes)
{
  const auto logs = create_event_logs();
  const auto bodies = create_tx_bodies(logs.size());
  ingest_committed_events(10, logs, bodies)();

  EventQuery events{};
  events.filters.push_back(
      EventFilter{.contract_addresses = {logs[0].events[0].contract_address},
                  .topics = {}});
  TransactionQuery transactions{};
  AddressTransactionQuery address{};
  address.address = bodies[0].signed_tx.tx.invocation.invoked_address;

  const auto invalid_for = [](HistoryCursorMethod method,
                              SHA256Hash const &query_hash) {
    EventCursorPayload payload{};
    payload.schema_version = cursor::kCursorVersion;
    payload.method = method;
    payload.query_hash = query_hash;
    payload.order = IterationOrder::IterationOrderAscending;
    payload.block_height = 10;
    payload.max_block_height = 10;
    std::vector<PaginationCursor> invalid{PaginationCursor(1),
                                          PaginationCursor(
                                              cursor::kMaxCursorBytes + 1)};
    const auto add = [&] {
      const auto bytes = x23::serialize(payload);
      PaginationCursor encoded;
      encoded.append_range(
          std::span<const std::uint8_t>(bytes.data(), bytes.size()));
      invalid.push_back(std::move(encoded));
    };
    payload.schema_version++;
    add();
    payload.schema_version = 2;
    add();
    payload.schema_version = cursor::kCursorVersion;
    payload.order = static_cast<IterationOrder>(2);
    add();
    payload.order = IterationOrder::IterationOrderAscending;
    if (method != HistoryCursorMethod::CursorEvents) {
      payload.sequence_number = 1;
      add();
    }
    return invalid;
  };
  for (auto const &bytes :
       invalid_for(HistoryCursorMethod::CursorEvents,
                   cursor::event_query_hash(events.filters))) {
    events.cursor = bytes;
    const auto page = db_->get_events(events);
    ASSERT_FALSE(page);
    EXPECT_EQ(page.error().code(), IndexerErrorCode::kInvalidCursor);
    EXPECT_EQ(block_explorer_->get_events(events)().outcome(),
              IndexerQueryOutcome::IndexerQueryInvalidCursor);
  }
  for (auto const &bytes :
       invalid_for(HistoryCursorMethod::CursorTransactions, SHA256Hash{})) {
    transactions.cursor = bytes;
    const auto page = db_->get_transactions(transactions);
    ASSERT_FALSE(page);
    EXPECT_EQ(page.error().code(), IndexerErrorCode::kInvalidCursor);
    EXPECT_EQ(block_explorer_->get_transactions(transactions)().outcome(),
              IndexerQueryOutcome::IndexerQueryInvalidCursor);
  }
  for (auto const &bytes :
       invalid_for(HistoryCursorMethod::CursorAddressTransactions,
                   cursor::address_query_hash(address.address))) {
    address.cursor = bytes;
    const auto page = db_->get_transactions_by_address(address);
    ASSERT_FALSE(page);
    EXPECT_EQ(page.error().code(), IndexerErrorCode::kInvalidCursor);
    EXPECT_EQ(block_explorer_->get_transactions_by_address(address)().outcome(),
              IndexerQueryOutcome::IndexerQueryInvalidCursor);
  }
}

TEST_F(IndexerTest, HistoryContinuationsWaitForTheirOriginalWatermark)
{
  ContractAddress addr{};
  std::ranges::fill(addr, 0xD7);
  auto logs = create_event_logs();
  auto bodies = create_tx_bodies(logs.size());
  for (size_t i = 0; i < logs.size(); ++i) {
    logs[i].events[0].contract_address = addr;
    // The test event does not carry topics.
    logs[i].events[0].num_topics = 0;
    bodies[i].signed_tx.tx.invocation.invoked_address = addr;
    logs[i].tx_hash = sha256(bodies[i].signed_tx.tx);
    ingest_committed_events(static_cast<BlockHeight>(10 + i),
                            TxEventLogBatch{logs[i]},
                            ProposedTransactionBatch{bodies[i]})();
  }
  for (const auto order : {IterationOrder::IterationOrderAscending,
                           IterationOrder::IterationOrderDescending}) {
    SCOPED_TRACE(static_cast<int>(order));
    const bool ascending = order == IterationOrder::IterationOrderAscending;
    EventQuery events{};
    events.filters.push_back(
        EventFilter{.contract_addresses = {addr}, .topics = {}});
    events.order = order;
    events.anchor.kind(ascending ? BlockAnchorKind::AnchorAtBeginning
                                 : BlockAnchorKind::AnchorAtEnd);
    events.limit = 1;
    TransactionQuery transactions{};
    transactions.order = order;
    transactions.anchor = events.anchor;
    transactions.limit = 1;
    AddressTransactionQuery address{};
    address.address = addr;
    address.order = order;
    address.anchor = events.anchor;
    address.limit = 1;
    const auto first_events = db_->get_events(events);
    ASSERT_TRUE(first_events && !first_events->cursor.empty());
    events.cursor = first_events->cursor;
    const auto first_txs = db_->get_transactions(transactions);
    ASSERT_TRUE(first_txs && !first_txs->cursor.empty());
    transactions.cursor = first_txs->cursor;
    const auto first_refs = db_->get_transactions_by_address(address);
    ASSERT_TRUE(first_refs && !first_refs->cursor.empty());
    address.cursor = first_refs->cursor;

    auto replacement =
        IndexDB::make_new(std::filesystem::path(tmp_dir_.directory()) /
                          (ascending ? "ascending" : "descending"));
    ASSERT_TRUE(replacement);
    ingest_committed_events(10, TxEventLogBatch{logs[0]},
                            ProposedTransactionBatch{bodies[0]},
                            &*replacement)();
    RecentTxFailureCache failures;
    IndexerBlockExplorerRpc rpc(*replacement, failures);
    auto wrong_events = events;
    wrong_events.filters[0].contract_addresses[0][0] ^= 0xFF;
    EXPECT_EQ(rpc.get_events(wrong_events)().outcome(),
              IndexerQueryOutcome::IndexerQueryInvalidCursor);
    auto wrong_address = address;
    wrong_address.address[0] ^= 0xFF;
    EXPECT_EQ(rpc.get_transactions_by_address(wrong_address)().outcome(),
              IndexerQueryOutcome::IndexerQueryInvalidCursor);
    auto wrong_method = transactions;
    wrong_method.cursor = events.cursor;
    EXPECT_EQ(rpc.get_transactions(wrong_method)().outcome(),
              IndexerQueryOutcome::IndexerQueryInvalidCursor);
    const auto missing_events = replacement->get_events(events);
    ASSERT_FALSE(missing_events);
    EXPECT_EQ(missing_events.error().code(),
              IndexerErrorCode::kHistoryUnavailable);
    const auto missing_txs = replacement->get_transactions(transactions);
    ASSERT_FALSE(missing_txs);
    EXPECT_EQ(missing_txs.error().code(),
              IndexerErrorCode::kHistoryUnavailable);
    const auto missing_refs = replacement->get_transactions_by_address(address);
    ASSERT_FALSE(missing_refs);
    EXPECT_EQ(missing_refs.error().code(),
              IndexerErrorCode::kHistoryUnavailable);
    EXPECT_EQ(rpc.get_events(events)().outcome(),
              IndexerQueryOutcome::IndexerQueryHistoryUnavailable);
    EXPECT_EQ(rpc.get_transactions(transactions)().outcome(),
              IndexerQueryOutcome::IndexerQueryHistoryUnavailable);
    EXPECT_EQ(rpc.get_transactions_by_address(address)().outcome(),
              IndexerQueryOutcome::IndexerQueryHistoryUnavailable);

    ingest_committed_events(11, TxEventLogBatch{logs[1]},
                            ProposedTransactionBatch{bodies[1]},
                            &*replacement)();
    const auto resumed_events = unwrap_page(rpc.get_events(events)());
    ASSERT_EQ(resumed_events.events.size(), 1);
    EXPECT_EQ(resumed_events.events[0].block_height, ascending ? 11 : 10);
    EXPECT_TRUE(resumed_events.cursor.empty());
    const auto resumed_txs = unwrap_page(rpc.get_transactions(transactions)());
    ASSERT_EQ(resumed_txs.transactions.size(), 1);
    EXPECT_EQ(resumed_txs.transactions[0].height, ascending ? 11 : 10);
    EXPECT_TRUE(resumed_txs.cursor.empty());
    const auto resumed_refs =
        unwrap_page(rpc.get_transactions_by_address(address)());
    ASSERT_EQ(resumed_refs.refs.size(), 1);
    EXPECT_EQ(resumed_refs.refs[0].height, ascending ? 11 : 10);
    EXPECT_TRUE(resumed_refs.cursor.empty());
  }
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

    TxEventLog log{};

    ProposedTransaction body{};
    body.signed_tx.tx.uniquifier.nonce = i;
    body.signed_tx.tx.invocation.invoked_address = addr;
    bodies.push_back(body);
    log.tx_hash = sha256(body.signed_tx.tx);
    logs.push_back(log); // present, but emits no events
  }

  // Whole batch ingested in a single call: events.size() == 4, but the total
  // number of emitted events across the batch is 0.
  ingest_committed_events(block_height, logs, bodies)();

  for (uint8_t i = 0; i < 4; i++) {
    AddressTransactionQuery q{};
    q.address = addrs[i];
    q.order = IterationOrder::IterationOrderAscending;
    q.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
    q.limit = 0;

    // Each eventless tx must still have produced its ROOT_TX row.
    AddressTransactionRefPage page =
        unwrap_page(block_explorer_->get_transactions_by_address(q)());
    ASSERT_EQ(page.refs.size(), 1);
    EXPECT_EQ(page.refs[0].tx_hash, logs[i].tx_hash);
  }
}

TEST_F(IndexerTest, RecentFailureVisibleThenCommittedRecordWins)
{
  TxEventLogBatch events = create_event_logs();
  ProposedTransactionBatch bodies = create_tx_bodies(events.size());
  const TransactionHash tx_hash = events[0].tx_hash;

  // Report a failure for a tx with no committed record.
  failure_cache_.report(tx_hash, make_failure_diag(100), *db_->latest_block());

  TxInfoResult result = block_explorer_->get_transaction_info(tx_hash)();
  ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoRecentFailure);
  EXPECT_EQ(result.recent_failure().attempt_height, 100);
  EXPECT_EQ(result.recent_failure().cause.conflict().code,
            DbConflictCode::ValueMismatch);

  // The retried tx commits at height 101: the committed record wins.
  ingest_legacy_events(101, events, bodies)();
  result = block_explorer_->get_transaction_info(tx_hash)();
  ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result.info().height, 101);
}

TEST_F(IndexerTest, RecentFailureExpiresAsWatermarkAdvances)
{
  TransactionHash failed_hash;
  std::ranges::fill(failed_hash, 0x7F);
  failure_cache_.report(failed_hash, make_failure_diag(100),
                        *db_->latest_block());

  // Ingest committed blocks 100..105; each ingest prunes against the new
  // watermark. Give each block one distinct committed tx.
  for (BlockHeight h = 100; h <= 105; h++) {
    TxEventLog log{};
    ProposedTransaction body{};
    body.signed_tx.tx.uniquifier.nonce = h;
    log.tx_hash = sha256(body.signed_tx.tx);
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
  TransactionHash tx_hash;
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

  auto ingest_at = [&](BlockHeight height) {
    TxEventLog log{};
    TxEmittedEvent ev;
    ev.contract_address = addr;
    ev.num_topics = 0;
    ev.serialized_event.resize(4);
    log.events.push_back(std::move(ev));

    ProposedTransaction body{};
    body.signed_tx.tx.invocation.invoked_address = addr;
    body.signed_tx.tx.uniquifier.nonce = height;
    log.tx_hash = sha256(body.signed_tx.tx);
    ingest_legacy_events(height, TxEventLogBatch{log},
                         ProposedTransactionBatch{body})();
    return log.tx_hash;
  };

  // Block 10 is contiguous; block 12 arrives before block 11, so it is
  // ingested but held behind the watermark at 10.
  const auto tx10 = ingest_at(10);
  const auto tx12 = ingest_at(12);
  ASSERT_EQ(*db().latest_block(), 10);

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

  EventPage event_page =
      unwrap_page(block_explorer_->get_events(event_query)());
  EXPECT_EQ(event_page.latest_block, 10);
  ASSERT_EQ(event_page.events.size(), 1);
  EXPECT_EQ(event_page.events[0].block_height, 10);
  EXPECT_EQ(event_page.events[0].tx_hash, tx10);

  // Scan 2 -- unfiltered transaction list: only block 10 appears.
  TransactionQuery tx_query{};
  tx_query.order = IterationOrder::IterationOrderAscending;
  tx_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  tx_query.limit = 0;

  TransactionPage tx_page =
      unwrap_page(block_explorer_->get_transactions(tx_query)());
  EXPECT_EQ(tx_page.latest_block, 10);
  ASSERT_EQ(tx_page.transactions.size(), 1);
  EXPECT_EQ(tx_page.transactions[0].height, 10);

  // Legacy rows cannot establish complete invocation-history coverage.
  AddressTransactionQuery addr_query{};
  addr_query.address = addr;
  addr_query.order = IterationOrder::IterationOrderAscending;
  addr_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  addr_query.limit = 0;

  EXPECT_EQ(
      block_explorer_->get_transactions_by_address(addr_query)().outcome(),
      IndexerQueryOutcome::IndexerQueryNotReady);
}

// A db-level failure must be reported as such, not as an empty page:
// otherwise a broken or not-yet-open index is indistinguishable from an
// empty chain (#3579).
TEST_F(IndexerTest, ClosedDbReportsNotReadyInsteadOfEmptyPages)
{
  // Moving the database out leaves *db_ closed while the block explorer
  // RPC still references it, mimicking a query racing a db that has not
  // opened (or has shut down).
  IndexDB moved_away = std::move(*db_);
  ASSERT_FALSE(db_->is_open());

  ContractAddress addr;
  std::ranges::fill(addr, 0xAA);

  EventQuery event_query{};
  EventFilter filter{};
  filter.contract_addresses.push_back(addr);
  event_query.filters.push_back(std::move(filter));
  event_query.order = IterationOrder::IterationOrderAscending;
  event_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  event_query.limit = 0;
  EventPageResult events = block_explorer_->get_events(event_query)();
  EXPECT_EQ(events.outcome(), IndexerQueryOutcome::IndexerQueryNotReady);

  TransactionQuery tx_query{};
  tx_query.order = IterationOrder::IterationOrderAscending;
  tx_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  tx_query.limit = 0;
  TransactionPageResult txs = block_explorer_->get_transactions(tx_query)();
  EXPECT_EQ(txs.outcome(), IndexerQueryOutcome::IndexerQueryNotReady);

  AddressTransactionQuery addr_query{};
  addr_query.address = addr;
  addr_query.order = IterationOrder::IterationOrderAscending;
  addr_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  addr_query.limit = 0;
  AddressTransactionRefPageResult refs =
      block_explorer_->get_transactions_by_address(addr_query)();
  EXPECT_EQ(refs.outcome(), IndexerQueryOutcome::IndexerQueryNotReady);

  // Block queries must also distinguish an unavailable database from a miss.
  BlockTransactionQuery block_query{};
  block_query.height = 10;
  block_query.order = IterationOrder::IterationOrderAscending;
  block_query.limit = 0;
  TransactionRefPageResult block_refs =
      block_explorer_->get_transactions_by_block(block_query)();
  EXPECT_EQ(block_refs.outcome(), IndexerQueryOutcome::IndexerQueryNotReady);
}

TEST_F(IndexerTest, EventRpcBoundsNonmatchingCandidatesAndResumes)
{
  ContractAddress address{};
  std::ranges::fill(address, 0xAA);
  ProposedTransactionBatch bodies;
  TxEventLogBatch logs;
  std::vector<SHA256Hash> hashes;
  for (uint32_t i = 0; i <= IndexDB::kEventsMaxCandidates; ++i) {
    ProposedTransaction body{};
    body.signed_tx.tx.uniquifier.nonce = i;
    body.signed_tx.tx.invocation.invoked_address = address;
    TxEventLog log{};
    log.tx_hash = sha256(body.signed_tx.tx);
    TxEmittedEvent event{};
    event.contract_address = address;
    log.events.push_back(event);
    hashes.push_back(log.tx_hash);
    bodies.push_back(body);
    logs.push_back(log);
  }
  ingest_legacy_events(10, logs, bodies)();
  std::ranges::sort(hashes);
  for (const auto order : {IterationOrder::IterationOrderAscending,
                           IterationOrder::IterationOrderDescending}) {
    const bool ascending = order == IterationOrder::IterationOrderAscending;
    EventQuery query{};
    query.order = order;
    query.anchor = BlockAnchor(ascending ? BlockAnchorKind::AnchorAtBeginning
                                         : BlockAnchorKind::AnchorAtEnd);
    EventFilter filter{};
    filter.contract_addresses.push_back(address);
    filter.topics.topic0.push_back(Topic{});
    query.filters.push_back(filter);
    const auto first = block_explorer().get_events(query)();
    ASSERT_EQ(first.outcome(), IndexerQueryOutcome::IndexerQueryOk);
    EXPECT_TRUE(first.page().events.empty());
    ASSERT_FALSE(first.page().cursor.empty());
    const auto &opaque = first.page().cursor;
    const auto cursor = cursor::decode(
        std::string_view(reinterpret_cast<const char *>(opaque.data()),
                         opaque.size()));
    if (!cursor) {
      ADD_FAILURE() << "Could not decode event cursor";
      return;
    }
    EXPECT_EQ(cursor->tx_hash,
              hashes[ascending ? IndexDB::kEventsMaxCandidates - 1 : 1]);
    // Even an internal caller cannot enlarge the server's hard budget.
    const auto oversized =
        db().get_events(query, IndexDB::kEventsMaxCandidates + 1);
    ASSERT_TRUE(oversized);
    EXPECT_EQ(oversized->cursor, first.page().cursor);
    query.cursor = first.page().cursor;
    const auto last = block_explorer().get_events(query)();
    ASSERT_EQ(last.outcome(), IndexerQueryOutcome::IndexerQueryOk);
    EXPECT_TRUE(last.page().events.empty());
    EXPECT_TRUE(last.page().cursor.empty());
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
