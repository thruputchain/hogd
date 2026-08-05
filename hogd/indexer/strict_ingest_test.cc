#include <expected>
#include <format>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <rocksdb/db.h> // NOLINT(misc-header-include-cycle)
#include <rocksdb/options.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "event.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/event_block_context.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/key_format.h"
#include "hogd/indexer/status.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "network.hh"
#include "pagination.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {
namespace {

ConsensusValueHash
test_genesis_hash()
{
  ConsensusValueHash hash;
  std::ranges::fill(hash, 0x42);
  return hash;
}

IndexerOutboxOrigin
make_origin(std::string_view protocol = "hogp/test",
            BlockHeight first_height = 1,
            ConsensusValueHash first_prev_hash = test_genesis_hash(),
            uint64_t epoch_sequence = 0)
{
  return IndexerOutboxOrigin{
      .protocol = ProtocolCommit(protocol.data(), protocol.size()),
      .epoch_sequence = epoch_sequence,
      .first_height = first_height,
      .first_prev_hash = first_prev_hash,
  };
}

ProposedTransaction
make_body(uint64_t nonce, uint8_t address_fill)
{
  ProposedTransaction body;
  body.signed_tx.tx.uniquifier.nonce = nonce;
  std::ranges::fill(body.signed_tx.tx.invocation.invoked_address, address_fill);
  body.nondeterministic_proposal_data.nondeterministic_bit_count =
      static_cast<uint32_t>(nonce);
  return body;
}

TxEventLog
make_log(const ProposedTransaction &body, std::optional<uint8_t> emitter_fill)
{
  TxEventLog log;
  log.proposed_tx_hash = sha256(body.signed_tx.tx);
  if (emitter_fill) {
    TxEmittedEvent event;
    std::ranges::fill(event.contract_address, *emitter_fill);
    event.serialized_event.push_back(*emitter_fill);
    log.events.push_back(std::move(event));
  }
  return log;
}

ProposalTxs
make_proposal_txs(const ProposedTransactionBatch &canonical_bodies)
{
  x23::XdrVec<SignedTransactionHash> hashes;
  x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
  for (const ProposedTransaction &body : canonical_bodies) {
    hashes.push_back(sha256(body.signed_tx.tx));
    nondeterministic_data.push_back(body.nondeterministic_proposal_data);
  }
  ProposalTxs proposal;
  proposal.hash = t_to_parallel_batch(hashes)();
  proposal.nondeterministic_data = t_to_parallel_batch(nondeterministic_data)();
  return proposal;
}

IndexerCommittedBlock
make_block(BlockHeight height, ConsensusValueHash prev_hash,
           ProposedTransactionBatch bodies = {},
           bool reverse_delivery_order = false)
{
  const ProposedTransactionBatch canonical_bodies = bodies;
  TxEventLogBatch logs;
  for (size_t i = 0; i < bodies.size(); ++i) {
    logs.push_back(make_log(bodies[i], i == 0 ? std::optional<uint8_t>(0xA0)
                                              : std::nullopt));
  }
  if (reverse_delivery_order) {
    std::ranges::reverse(bodies);
    std::ranges::reverse(logs);
  }
  ProposalTxs proposal = make_proposal_txs(canonical_bodies);
  ConsensusValue value;
  value.prev_hash = prev_hash;
  value.proposal_header.block_height = height;
  value.proposal_header.block_timepoint = 1000 + height;
  value.proposal_header.events_hash = compute_indexer_outbox_event_root(logs);
  auto txs_root = compute_indexer_proposal_txs_root(proposal)();
  EXPECT_TRUE(txs_root.has_value()) << txs_root.error();
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal);
  IndexerCommittedBlockPayload payload{
      .format_version = kIndexerOutboxFormatVersion,
      .consensus_value = std::move(value),
      .proposal_txs = std::move(proposal),
      .event_logs = std::move(logs),
      .tx_bodies = std::move(bodies),
  };
  auto block = make_indexer_committed_block(payload)();
  EXPECT_TRUE(block.has_value()) << block.error();
  return std::move(*block);
}

void
refresh_identity(IndexerCommittedBlock &block)
{
  block.identity.consensus_hash = sha256(block.payload.consensus_value);
  block.identity.payload_digest = sha256(block.payload);
}

void
overwrite_column_family(const std::filesystem::path &path,
                        std::string_view column_family, std::string_view key,
                        std::string_view value)
{
  rocksdb::Options options;
  std::vector<std::string> names;
  ASSERT_TRUE(
      rocksdb::DB::ListColumnFamilies(options, path.string(), &names).ok());
  std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
  descriptors.reserve(names.size());
  for (const std::string &name : names) {
    descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions(options));
  }
  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  rocksdb::DB *raw = nullptr;
  ASSERT_TRUE(
      rocksdb::DB::Open(options, path.string(), descriptors, &handles, &raw)
          .ok());
  ASSERT_NE(raw, nullptr);
  auto selected = std::ranges::find_if(handles, [&](const auto *handle) {
    return handle->GetName() == column_family;
  });
  ASSERT_NE(selected, handles.end());
  ASSERT_TRUE(raw->Put(rocksdb::WriteOptions(), *selected,
                       rocksdb::Slice(key.data(), key.size()),
                       rocksdb::Slice(value.data(), value.size()))
                  .ok());
  for (rocksdb::ColumnFamilyHandle *handle : handles) {
    ASSERT_TRUE(raw->DestroyColumnFamilyHandle(handle).ok());
  }
  delete raw;
}

void
overwrite_default_metadata(const std::filesystem::path &path,
                           std::string_view key, std::string_view value)
{
  overwrite_column_family(path, rocksdb::kDefaultColumnFamilyName, key, value);
}

class StrictIndexerTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    std::filesystem::create_directories(dir_.directory());
    path_ = std::filesystem::path(dir_.directory()) / "db";
    auto opened = IndexDB::make_new(path_);
    ASSERT_TRUE(opened.has_value());
    db_ = std::make_unique<IndexDB>(std::move(*opened));
  }

  TmpDir dir_;
  std::filesystem::path path_;
  std::unique_ptr<IndexDB> db_;
};

TEST_F(StrictIndexerTest, FreshOriginExactRepeatAndBoundaryTransition)
{
  EXPECT_EQ(db_->durable_progress().state(),
            IndexerProgressState::IndexerProgressUninitialized);
  IndexerOutboxOrigin origin = make_origin();
  IndexerEpochResult initialized = db_->initialize_epoch(origin);
  ASSERT_EQ(initialized.outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  EXPECT_EQ(initialized.progress().contiguous_height, 0);
  EXPECT_EQ(initialized.progress().last_consensus_hash, origin.first_prev_hash);

  EXPECT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochUnchanged);
  EXPECT_EQ(db_->initialize_epoch(
                   make_origin("hogp/next", 2, origin.first_prev_hash))
                .outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);

  IndexerCommittedBlock first = make_block(1, origin.first_prev_hash);
  ASSERT_EQ(db_->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
  IndexerOutboxOrigin next =
      make_origin("hogp/next", 2, first.identity.consensus_hash, 1);
  IndexerEpochResult transitioned = db_->initialize_epoch(next);
  ASSERT_EQ(transitioned.outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  EXPECT_EQ(transitioned.progress().contiguous_height, 1);
  EXPECT_EQ(transitioned.progress().last_payload_digest,
            first.identity.payload_digest);

  EXPECT_EQ(db_->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockOriginMismatch);

  IndexerOutboxOrigin reused =
      make_origin("hogp/test", 2, first.identity.consensus_hash, 2);
  EXPECT_EQ(db_->initialize_epoch(reused).outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);
}

TEST_F(StrictIndexerTest, CommittedValidationAwaitsBeforeIngestMutex)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ProposedTransaction body = make_body(101, 0xa1);
  IndexerCommittedBlock block = make_block(1, origin.first_prev_hash, {body});

  auto apply_from_pool = [&]() -> x23::Async<IndexerBlockApplyResult> {
    co_await default_threadpool().bg();
    co_return co_await db_->apply_committed_block(origin, block);
  };

  EXPECT_EQ(apply_from_pool()().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
}

TEST_F(StrictIndexerTest, ConcurrentValidationSerializesDurableMutation)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ProposedTransaction body = make_body(103, 0xa3);
  IndexerCommittedBlock block = make_block(1, origin.first_prev_hash, {body});
  std::latch validation_ready(2);
  db_->set_apply_validation_test_hook([&] {
    validation_ready.count_down();
    validation_ready.wait();
  });
  auto apply = [&] {
    return db_->apply_committed_block(origin, block)().outcome();
  };
  auto first = std::async(std::launch::async, apply);
  auto second = std::async(std::launch::async, apply);

  const IndexerBlockApplyOutcome first_outcome = first.get();
  const IndexerBlockApplyOutcome second_outcome = second.get();

  EXPECT_TRUE(
      (first_outcome == IndexerBlockApplyOutcome::IndexerBlockApplied &&
       second_outcome == IndexerBlockApplyOutcome::IndexerBlockDuplicate) ||
      (second_outcome == IndexerBlockApplyOutcome::IndexerBlockApplied &&
       first_outcome == IndexerBlockApplyOutcome::IndexerBlockDuplicate));
  EXPECT_TRUE(db_->get_tx_record(sha256(body.signed_tx.tx)).has_value());
}

TEST_F(StrictIndexerTest, FreshOriginRejectsMidchainAndZeroGenesisBoundary)
{
  EXPECT_EQ(db_->initialize_epoch(make_origin("hogp/test", 101)).outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);
  EXPECT_EQ(db_->initialize_epoch(make_origin("hogp/test", 1, {})).outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);
  EXPECT_EQ(db_->durable_progress().state(),
            IndexerProgressState::IndexerProgressUninitialized);
}

TEST_F(StrictIndexerTest, TransitionReopensBeforeFirstNewProtocolBlock)
{
  IndexerOutboxOrigin first_origin = make_origin("hogp/a");
  ASSERT_EQ(db_->initialize_epoch(first_origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  IndexerCommittedBlock first = make_block(1, first_origin.first_prev_hash);
  ASSERT_EQ(db_->apply_committed_block(first_origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
  IndexerOutboxOrigin second_origin =
      make_origin("hogp/b", 2, first.identity.consensus_hash, 1);
  ASSERT_EQ(db_->initialize_epoch(second_origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);

  db_.reset();
  auto reopened = IndexDB::open(path_);
  ASSERT_TRUE(reopened.has_value());
  IndexerProgress progress = reopened->durable_progress();
  ASSERT_EQ(progress.state(), IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(progress.initialized().origin, second_origin);
  EXPECT_EQ(progress.initialized().contiguous_height, 1);
  EXPECT_EQ(progress.initialized().last_payload_digest,
            first.identity.payload_digest);

  IndexerCommittedBlock second = make_block(2, first.identity.consensus_hash);
  EXPECT_EQ(reopened->apply_committed_block(second_origin, second)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
}

TEST_F(StrictIndexerTest, DifferentOriginCannotSupersedeUnusedEpochAcrossReopen)
{
  IndexerOutboxOrigin first = make_origin("hogp/a");
  IndexerOutboxOrigin different = make_origin("hogp/b");
  ASSERT_EQ(db_->initialize_epoch(first).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  EXPECT_EQ(db_->initialize_epoch(first).outcome(),
            IndexerEpochOutcome::IndexerEpochUnchanged);
  EXPECT_EQ(db_->initialize_epoch(different).outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);

  db_.reset();
  auto reopened = IndexDB::open(path_);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->initialize_epoch(first).outcome(),
            IndexerEpochOutcome::IndexerEpochUnchanged);
  EXPECT_EQ(reopened->initialize_epoch(different).outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);
  EXPECT_EQ(reopened->durable_progress().initialized().origin, first);
}

TEST_F(StrictIndexerTest,
       ConsecutiveEmptyEpochsRequireAdjacentSequenceAndCanonicalBoundary)
{
  const ConsensusValueHash boundary = test_genesis_hash();
  IndexerOutboxOrigin epoch_a = make_origin("hogp/a", 1, boundary, 0);
  IndexerOutboxOrigin epoch_b = make_origin("hogp/b", 1, boundary, 1);
  IndexerOutboxOrigin epoch_c = make_origin("hogp/c", 1, boundary, 2);
  ASSERT_EQ(db_->initialize_epoch(epoch_a).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ASSERT_EQ(db_->initialize_epoch(epoch_b).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);

  EXPECT_EQ(db_->initialize_epoch(
                   make_origin("hogp/wrong-sequence", 1, boundary, 1))
                .outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);
  ConsensusValueHash wrong_boundary = boundary;
  wrong_boundary[0] ^= 0xff;
  EXPECT_EQ(db_->initialize_epoch(
                   make_origin("hogp/wrong-boundary", 1, wrong_boundary, 2))
                .outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);
  EXPECT_EQ(
      db_->initialize_epoch(make_origin("hogp/a", 1, boundary, 2)).outcome(),
      IndexerEpochOutcome::IndexerEpochConflict);
  ASSERT_EQ(db_->initialize_epoch(epoch_c).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);

  IndexerCommittedBlock first = make_block(1, boundary);
  EXPECT_EQ(db_->apply_committed_block(epoch_a, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockOriginMismatch);
  EXPECT_EQ(db_->apply_committed_block(epoch_b, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockOriginMismatch);

  db_.reset();
  auto reopened = IndexDB::open(path_);
  ASSERT_TRUE(reopened.has_value());
  ASSERT_EQ(reopened->durable_progress().state(),
            IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(reopened->durable_progress().initialized().origin, epoch_c);
  EXPECT_EQ(reopened->apply_committed_block(epoch_c, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
}

TEST_F(StrictIndexerTest, OrderedApplySupportsEventlessRootAndEmptyBlock)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ProposedTransaction eventless = make_body(7, 0xCC);
  IndexerCommittedBlock first =
      make_block(1, origin.first_prev_hash,
                 ProposedTransactionBatch{make_body(6, 0xBB), eventless}, true);
  ASSERT_EQ(db_->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);

  const SignedTransactionHash eventless_hash = sha256(eventless.signed_tx.tx);
  auto record = db_->get_tx_record(eventless_hash);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->events.events.empty());
  AddressTransactionQuery query{};
  query.address = eventless.signed_tx.tx.invocation.invoked_address;
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  auto page = db_->get_transactions_by_address(query);
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->refs.size(), 1);
  EXPECT_EQ(page->refs[0].tx_hash, eventless_hash);

  IndexerCommittedBlock empty = make_block(2, first.identity.consensus_hash);
  ASSERT_EQ(db_->apply_committed_block(origin, empty)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
  IndexerProgress progress = db_->durable_progress();
  ASSERT_EQ(progress.state(), IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(progress.initialized().contiguous_height, 2);
  EXPECT_EQ(progress.initialized().last_payload_digest,
            empty.identity.payload_digest);
}

TEST_F(StrictIndexerTest, DuplicateLedgerAndFutureGapWriteNothing)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ProposedTransaction future_body = make_body(9, 0xD0);
  IndexerCommittedBlock future =
      make_block(2, {}, ProposedTransactionBatch{future_body});
  IndexerBlockApplyResult gap = db_->apply_committed_block(origin, future)();
  ASSERT_EQ(gap.outcome(), IndexerBlockApplyOutcome::IndexerBlockGap);
  EXPECT_EQ(gap.gap().expected_height, 1);
  EXPECT_FALSE(
      db_->get_tx_record(sha256(future_body.signed_tx.tx)).has_value());

  IndexerCommittedBlock first = make_block(1, origin.first_prev_hash);
  ASSERT_EQ(db_->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
  IndexerCommittedBlock second = make_block(2, first.identity.consensus_hash);
  ASSERT_EQ(db_->apply_committed_block(origin, second)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
  EXPECT_EQ(db_->apply_committed_block(origin, second)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockDuplicate);
  EXPECT_EQ(db_->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockDuplicate);

  IndexerCommittedBlock conflict = first;
  conflict.payload.consensus_value.proposal_header.block_timepoint++;
  refresh_identity(conflict);
  EXPECT_EQ(db_->apply_committed_block(origin, conflict)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockContentConflict);
}

TEST_F(StrictIndexerTest, RejectsPrevProposalBodyEventRootAndDigestConflicts)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ProposedTransaction body = make_body(5, 0xE0);
  const IndexerCommittedBlock valid =
      make_block(1, origin.first_prev_hash, ProposedTransactionBatch{body});

  auto expect_conflict = [&](const IndexerCommittedBlock &block) {
    EXPECT_EQ(db_->apply_committed_block(origin, block)().outcome(),
              IndexerBlockApplyOutcome::IndexerBlockContentConflict);
    EXPECT_EQ(db_->durable_progress().initialized().contiguous_height, 0);
  };

  IndexerCommittedBlock prev = valid;
  prev.payload.consensus_value.prev_hash[0] ^= 1;
  refresh_identity(prev);
  expect_conflict(prev);

  IndexerCommittedBlock negative = valid;
  negative.payload.consensus_value.proposal_header.block_height = -1;
  refresh_identity(negative);
  expect_conflict(negative);

  IndexerCommittedBlock proposal = valid;
  proposal.payload.consensus_value.proposal_txs_hash[0] ^= 1;
  refresh_identity(proposal);
  expect_conflict(proposal);

  IndexerCommittedBlock body_conflict = valid;
  body_conflict.payload.tx_bodies[0].signed_tx.tx.uniquifier.nonce++;
  refresh_identity(body_conflict);
  expect_conflict(body_conflict);

  IndexerCommittedBlock event_conflict = valid;
  event_conflict.payload.event_logs[0].proposed_tx_hash[0] ^= 1;
  event_conflict.payload.consensus_value.proposal_header.events_hash =
      compute_indexer_outbox_event_root(event_conflict.payload.event_logs);
  refresh_identity(event_conflict);
  expect_conflict(event_conflict);

  IndexerCommittedBlock root = valid;
  root.payload.consensus_value.proposal_header.events_hash[0] ^= 1;
  refresh_identity(root);
  expect_conflict(root);

  IndexerCommittedBlock txs_root = valid;
  txs_root.payload.consensus_value.proposal_header.txs_hash[0] ^= 1;
  refresh_identity(txs_root);
  expect_conflict(txs_root);

  IndexerCommittedBlock digest = valid;
  digest.identity.payload_digest[0] ^= 1;
  expect_conflict(digest);

  IndexerCommittedBlock missing_proposal = valid;
  missing_proposal.payload.proposal_txs = {};
  missing_proposal.payload.consensus_value.proposal_txs_hash =
      proposal_txs_compatibility_hash(missing_proposal.payload.proposal_txs);
  refresh_identity(missing_proposal);
  expect_conflict(missing_proposal);
}

TEST_F(StrictIndexerTest, ReopenRestoresExactProgressAndRejectsCorruption)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  IndexerCommittedBlock first = make_block(1, origin.first_prev_hash);
  ASSERT_EQ(db_->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
  const IndexerInitializedProgress before =
      db_->durable_progress().initialized();

  db_.reset();
  auto reopened = IndexDB::open(path_);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(x23::serialize(reopened->durable_progress().initialized()),
            x23::serialize(before));
  reopened = std::unexpected(IndexerError(IndexerErrorCode::kInternal));

  overwrite_default_metadata(path_, IndexerKeyFormat::kDurableProgressMetaKey,
                             "malformed");
  EXPECT_FALSE(IndexDB::open(path_).has_value());
}

TEST_F(StrictIndexerTest, ReopenRejectsMalformedPerHeightIdentity)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  IndexerCommittedBlock first = make_block(1, origin.first_prev_hash);
  ASSERT_EQ(db_->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);
  db_.reset();

  overwrite_default_metadata(path_,
                             IndexerKeyFormat::make_block_identity_meta_key(1),
                             "malformed");
  EXPECT_FALSE(IndexDB::open(path_).has_value());
}

TEST_F(StrictIndexerTest, MissingMetadataWithAnyIndexRowsFailsClosed)
{
  db_.reset();
  for (unsigned int mask = 1; mask <= 3; ++mask) {
    const std::filesystem::path path =
        std::filesystem::path(dir_.directory()) / std::to_string(mask);
    {
      auto fresh = IndexDB::make_new(path);
      ASSERT_TRUE(fresh.has_value());
    }
    if ((mask & 1U) != 0) {
      overwrite_column_family(path, IndexerKeyFormat::kTxIndexColumnFamily,
                              std::string(32, '\x11'), "crash-left-tx");
    }
    if ((mask & 2U) != 0) {
      overwrite_column_family(path,
                              IndexerKeyFormat::kContractIndexColumnFamily,
                              "crash-left-address", "");
    }
    EXPECT_FALSE(IndexDB::open(path).has_value()) << "mask " << mask;
    EXPECT_FALSE(IndexDB::open_or_make_new(path).has_value())
        << "mask " << mask;
  }

  const std::filesystem::path default_only =
      std::filesystem::path(dir_.directory()) / "default-only";
  {
    auto fresh = IndexDB::make_new(default_only);
    ASSERT_TRUE(fresh.has_value());
  }
  overwrite_default_metadata(default_only, "crash-left-default", "residue");
  EXPECT_FALSE(IndexDB::open(default_only).has_value());
}

TEST_F(StrictIndexerTest, MalformedLegacyWatermarkFailsClosed)
{
  db_.reset();
  overwrite_default_metadata(path_,
                             IndexerKeyFormat::kHighestIndexedBlockMetaKey,
                             "malformed");
  EXPECT_FALSE(IndexDB::open(path_).has_value());
  EXPECT_FALSE(IndexDB::open_or_make_new(path_).has_value());
}

TEST_F(StrictIndexerTest, RecognizedLegacyDataReopensAndRefusesStrictMigration)
{
  ProposedTransaction first_body = make_body(70, 0x70);
  TxEventLog first_log = make_log(first_body, 0x71);
  ASSERT_TRUE(db_->consume_events(1, TxEventLogBatch{first_log},
                                  ProposedTransactionBatch{first_body})
                  .has_value());
  db_.reset();

  auto reopened = IndexDB::open(path_);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->latest_block(), 1);
  EXPECT_TRUE(
      reopened->get_tx_record(sha256(first_body.signed_tx.tx)).has_value());
  EXPECT_EQ(reopened->initialize_epoch(make_origin()).outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);

  ProposedTransaction second_body = make_body(71, 0x72);
  TxEventLog second_log = make_log(second_body, 0x73);
  EXPECT_TRUE(reopened
                  ->consume_events(2, TxEventLogBatch{second_log},
                                   ProposedTransactionBatch{second_body})
                  .has_value());
  EXPECT_EQ(reopened->latest_block(), 2);
}

TEST_F(StrictIndexerTest, LegacyWatermarkWithStrictMetadataResidueFailsClosed)
{
  db_.reset();
  const std::vector<std::pair<std::string, std::string>> residue = {
      {"block identity", IndexerKeyFormat::make_block_identity_meta_key(1)},
      {"epoch origin/history",
       IndexerKeyFormat::make_epoch_history_meta_key(make_origin().protocol)},
      {"unknown strict metadata",
       std::string(IndexerKeyFormat::kStrictMetadataPrefix) + "orphan"},
  };

  for (size_t i = 0; i < residue.size(); ++i) {
    SCOPED_TRACE(residue[i].first);
    const std::filesystem::path path =
        std::filesystem::path(dir_.directory()) / std::format("legacy-{}", i);
    {
      auto legacy = IndexDB::make_new(path);
      ASSERT_TRUE(legacy.has_value());
      ProposedTransaction body = make_body(90 + i, 0x90 + i);
      TxEventLog log = make_log(body, 0xA0 + i);
      ASSERT_TRUE(legacy
                      ->consume_events(1, TxEventLogBatch{log},
                                       ProposedTransactionBatch{body})
                      .has_value());
    }
    overwrite_default_metadata(path, residue[i].second, "orphan");
    EXPECT_FALSE(IndexDB::open(path).has_value());
    EXPECT_FALSE(IndexDB::open_or_make_new(path).has_value());
  }
}

TEST_F(StrictIndexerTest, LegacyIngestExcludesEpochInitializationUntilComplete)
{
  std::latch ingest_reserved(1);
  std::latch allow_ingest(1);
  std::latch initialize_started(1);
  db_->set_legacy_ingest_test_hook([&] {
    ingest_reserved.count_down();
    allow_ingest.wait();
  });
  ProposedTransaction body = make_body(70, 0x70);
  TxEventLog log = make_log(body, 0x71);
  IndexerStatus<void> ingest_result;
  std::thread ingest([&] {
    ingest_result = db_->consume_events(1, TxEventLogBatch{log},
                                        ProposedTransactionBatch{body});
  });
  ingest_reserved.wait();
  IndexerEpochResult initialize_result;
  std::thread initialize([&] {
    initialize_started.count_down();
    initialize_result = db_->initialize_epoch(make_origin());
  });
  initialize_started.wait();
  allow_ingest.count_down();
  ingest.join();
  initialize.join();
  ASSERT_TRUE(ingest_result.has_value());
  EXPECT_EQ(initialize_result.outcome(),
            IndexerEpochOutcome::IndexerEpochConflict);
  EXPECT_EQ(db_->durable_progress().state(),
            IndexerProgressState::IndexerProgressUninitialized);
}

TEST_F(StrictIndexerTest, DurableInitializationExcludesLegacyIngest)
{
  std::latch initialize_reserved(1);
  std::latch allow_initialize(1);
  std::latch ingest_started(1);
  db_->set_initialize_epoch_test_hook([&] {
    initialize_reserved.count_down();
    allow_initialize.wait();
  });

  IndexerEpochResult initialize_result;
  std::thread initialize(
      [&] { initialize_result = db_->initialize_epoch(make_origin()); });
  initialize_reserved.wait();
  ProposedTransaction body = make_body(80, 0x80);
  TxEventLog log = make_log(body, 0x81);
  IndexerStatus<void> ingest_result;
  std::thread ingest([&] {
    ingest_started.count_down();
    ingest_result = db_->consume_events(1, TxEventLogBatch{log},
                                        ProposedTransactionBatch{body});
  });
  ingest_started.wait();
  allow_initialize.count_down();
  initialize.join();
  ingest.join();

  ASSERT_EQ(initialize_result.outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  EXPECT_FALSE(ingest_result.has_value());
  EXPECT_FALSE(db_->get_tx_record(sha256(body.signed_tx.tx)).has_value());
}

TEST_F(StrictIndexerTest, HistoricalIdentitiesAreValidatedLazily)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ConsensusValueHash previous = origin.first_prev_hash;
  IndexerCommittedBlock first;
  for (BlockHeight height = 1; height <= 100; ++height) {
    IndexerCommittedBlock block = make_block(height, previous);
    ASSERT_EQ(db_->apply_committed_block(origin, block)().outcome(),
              IndexerBlockApplyOutcome::IndexerBlockApplied);
    previous = block.identity.consensus_hash;
    if (height == 1) {
      first = block;
    }
  }
  db_.reset();
  overwrite_default_metadata(path_,
                             IndexerKeyFormat::make_block_identity_meta_key(1),
                             "malformed");

  auto reopened = IndexDB::open(path_);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->durable_progress().initialized().contiguous_height, 100);
  EXPECT_EQ(reopened->apply_committed_block(origin, first)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockInternalFailure);
}

TEST_F(StrictIndexerTest, AtomicBatchNeverMakesOnePointRowVisibleAlone)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ProposedTransactionBatch bodies;
  for (uint64_t i = 0; i < 256; ++i) {
    bodies.push_back(make_body(100 + i, static_cast<uint8_t>(i)));
  }
  const SignedTransactionHash first_hash = sha256(bodies.front().signed_tx.tx);
  const SignedTransactionHash last_hash = sha256(bodies.back().signed_tx.tx);
  IndexerCommittedBlock block =
      make_block(1, origin.first_prev_hash, std::move(bodies));
  std::atomic<bool> done = false;
  std::thread writer([&] {
    EXPECT_EQ(db_->apply_committed_block(origin, block)().outcome(),
              IndexerBlockApplyOutcome::IndexerBlockApplied);
    done.store(true, std::memory_order_release);
  });
  while (!done.load(std::memory_order_acquire)) {
    if (db_->get_tx_record(last_hash).has_value()) {
      EXPECT_TRUE(db_->get_tx_record(first_hash).has_value());
      EXPECT_EQ(db_->durable_progress().initialized().contiguous_height, 1);
    }
  }
  writer.join();
  EXPECT_TRUE(db_->get_tx_record(first_hash).has_value());
  EXPECT_TRUE(db_->get_tx_record(last_hash).has_value());
}

TEST_F(StrictIndexerTest,
       SuccessfulStrictApplyReopensAllRowsAndProgressTogether)
{
  IndexerOutboxOrigin origin = make_origin();
  ASSERT_EQ(db_->initialize_epoch(origin).outcome(),
            IndexerEpochOutcome::IndexerEpochInitialized);
  ProposedTransaction first = make_body(900, 0x90);
  ProposedTransaction second = make_body(901, 0x91);
  IndexerCommittedBlock block =
      make_block(1, origin.first_prev_hash,
                 ProposedTransactionBatch{first, second});
  ASSERT_EQ(db_->apply_committed_block(origin, block)().outcome(),
            IndexerBlockApplyOutcome::IndexerBlockApplied);

  // Indexed rows and durable progress must reopen at the same committed height.
  db_.reset();
  auto reopened = IndexDB::open(path_);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_TRUE(reopened->get_tx_record(sha256(first.signed_tx.tx)).has_value());
  EXPECT_TRUE(reopened->get_tx_record(sha256(second.signed_tx.tx)).has_value());
  IndexerProgress progress = reopened->durable_progress();
  ASSERT_EQ(progress.state(), IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(progress.initialized().contiguous_height, 1);
  EXPECT_EQ(progress.initialized().last_consensus_hash,
            block.identity.consensus_hash);
  EXPECT_EQ(progress.initialized().last_payload_digest,
            block.identity.payload_digest);
}

} // namespace
} // namespace hogd
