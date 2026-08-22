#include <array>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <latch>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "event.hh"
#include "event_outbox_internal.hh"
#include "exec_result.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/event_block_context.h"
#include "hogd/hog_event/indexer_outbox.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "indexer_payload_validation.h"
#include "parallel.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/fileio.h"
#include "x23/rpc_common.h"
#include "x23/rpc_msg.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {
namespace {

ProposedTransaction
make_body(uint64_t nonce)
{
  ProposedTransaction body;
  body.signed_tx.tx.uniquifier.block_expiry_height = 1000 + nonce;
  body.signed_tx.tx.uniquifier.nonce = nonce;
  body.signed_tx.tx.invocation.gas_limit = 1'000'000 + nonce;
  body.nondeterministic_proposal_data.nondeterministic_bits.push_back(
      static_cast<uint8_t>(nonce));
  body.nondeterministic_proposal_data.nondeterministic_bit_count = 8;
  return body;
}

TxEventLog
make_event(const ProposedTransaction &body, uint8_t marker,
           bool with_emitted_event = true)
{
  TxEventLog event;
  event.tx_hash = sha256(body.signed_tx.tx);
  if (with_emitted_event) {
    TxEmittedEvent emitted;
    emitted.contract_address[0] = marker;
    emitted.serialized_event.push_back(marker);
    event.events.push_back(std::move(emitted));
  }
  return event;
}

ProposalTxs
make_canonical_proposal_txs(const ProposedTransactionBatch &proposal_bodies)
{
  x23::XdrVec<TransactionHash> hashes;
  x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
  for (const ProposedTransaction &body : proposal_bodies) {
    hashes.push_back(sha256(body.signed_tx.tx));
    nondeterministic_data.push_back(body.nondeterministic_proposal_data);
  }

  ProposalTxs proposal_txs;
  const size_t split = std::min<size_t>(2, hashes.size());
  if (split != 0) {
    append_to_parallel_batch(x23::span<const TransactionHash>(hashes.data(),
                                                              split),
                             proposal_txs.hash)();
    append_to_parallel_batch(x23::span<const NondeterministicProposalData>(
                                 nondeterministic_data.data(), split),
                             proposal_txs.nondeterministic_data)();
  }
  if (split != hashes.size()) {
    append_to_parallel_batch(x23::span<const TransactionHash>(
                                 hashes.data() + split, hashes.size() - split),
                             proposal_txs.hash)();
    append_to_parallel_batch(x23::span<const NondeterministicProposalData>(
                                 nondeterministic_data.data() + split,
                                 nondeterministic_data.size() - split),
                             proposal_txs.nondeterministic_data)();
  }
  return proposal_txs;
}

void
sort_event_body_batches(ProposedTransactionBatch &bodies,
                        TxEventLogBatch &events)
{
  if (bodies.size() != events.size()) {
    return;
  }
  EventWithBodyBatch pairs;
  pairs.reserve(bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i) {
    pairs.push_back(
        {.event = std::move(events[i]), .body = std::move(bodies[i])});
  }
  std::ranges::sort(pairs, [](const auto &lhs, const auto &rhs) {
    return lhs.event.tx_hash < rhs.event.tx_hash;
  });
  events.clear();
  bodies.clear();
  events.reserve(pairs.size());
  bodies.reserve(pairs.size());
  for (auto &pair : pairs) {
    events.push_back(std::move(pair.event));
    bodies.push_back(std::move(pair.body));
  }
}

IndexerOutboxCandidatePayload
make_candidate(BlockID block_id, BlockHeight height,
               ProposedTransactionBatch bodies, TxEventLogBatch events)
{
  sort_event_body_batches(bodies, events);
  const EventsHash event_root =
      compute_indexer_outbox_event_root(events, bodies);
  return IndexerOutboxCandidatePayload{
      .format_version = kIndexerOutboxFormatVersion,
      .block_id = block_id,
      .block_height = height,
      .event_root = event_root,
      .event_logs = std::move(events),
      .tx_bodies = std::move(bodies),
  };
}

IndexerOutboxCommittedPayload
make_committed_with_proposal(BlockHeight height,
                             const ProposalTxs &proposal_txs,
                             ProposedTransactionBatch bodies,
                             TxEventLogBatch events)
{
  sort_event_body_batches(bodies, events);
  ConsensusValue value;
  value.proposal_header.block_height = height;
  value.proposal_header.events_hash =
      compute_indexer_outbox_event_root(events, bodies);
  auto txs_root = compute_indexer_proposal_txs_root(proposal_txs)();
  CHECK(txs_root.has_value());
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_header.block_timepoint = 100 + height;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal_txs);
  return IndexerOutboxCommittedPayload{
      .format_version = kIndexerOutboxFormatVersion,
      .consensus_value = std::move(value),
      .event_logs = std::move(events),
      .tx_bodies = std::move(bodies),
  };
}

IndexerOutboxCommittedPayload
make_committed(BlockHeight height, ProposedTransactionBatch bodies,
               TxEventLogBatch events)
{
  ProposalTxs proposal_txs = make_canonical_proposal_txs(bodies);
  return make_committed_with_proposal(height, std::move(proposal_txs),
                                      std::move(bodies), std::move(events));
}

IndexerCommittedBlockPayload
make_committed_payload_golden_fixture()
{
  IndexerCommittedBlockPayload payload;
  payload.format_version = kIndexerOutboxFormatVersion;

  ConsensusValue &value = payload.consensus_value;
  value.protocol = 0x01020304;
  value.prev_hash.fill(0);
  value.prev_hash[0] = 0x10;
  value.prev_hash[31] = 0x2f;
  value.proposal_header.network_hash.fill(0);
  value.proposal_header.network_hash[0] = 0x30;
  value.proposal_header.network_hash[31] = 0x4f;
  value.proposal_header.genesis_configuration_hash.fill(0);
  value.proposal_header.proposer.fill(0);
  value.proposal_header.proposer[0] = 0x50;
  value.proposal_header.proposer[31] = 0x6f;
  value.proposal_header.block_timepoint = 0x0102030405060708;
  value.proposal_header.block_height = 123456789;
  value.proposal_header.storage_hash.fill(0);
  value.proposal_header.storage_hash[0] = 0x90;
  value.proposal_header.storage_hash[31] = 0xaf;
  value.upgrades = {{0xde, 0xad, 0xbe, 0xef}, {1, 2, 3}};

  TxEventLog event;
  event.tx_hash.fill(0);
  event.tx_hash[0] = 0x21;
  event.tx_hash[31] = 0x40;
  event.exec_result = ExecResult::ExecResultTxRootFrameReverted;
  event.root_output.status_code(InvocationFrameStatusCode::Abort);
  event.root_output.abort_buffer() = {0xfa, 0xce, 0xb0, 0x0c, 0x01};
  TxEmittedEvent emitted;
  emitted.contract_address.fill(0);
  emitted.contract_address[0] = 0x41;
  emitted.contract_address[31] = 0x60;
  emitted.num_topics = 1;
  emitted.serialized_event.resize(35);
  emitted.serialized_event[0] = 0x61;
  emitted.serialized_event[31] = 0x80;
  emitted.serialized_event[32] = 0xca;
  emitted.serialized_event[33] = 0xfe;
  emitted.serialized_event[34] = 0x01;
  event.events.push_back(std::move(emitted));
  payload.event_logs.push_back(std::move(event));

  ProposedTransaction body;
  body.signed_tx.tx.invocation.invoked_address.fill(0);
  body.signed_tx.tx.invocation.invoked_address[0] = 0x81;
  body.signed_tx.tx.invocation.invoked_address[31] = 0xa0;
  body.signed_tx.tx.invocation.entrypoint = {'g', 'o', 'l', 'd',
                                             'e', 'n', '0', '1'};
  body.signed_tx.tx.invocation.gas_limit = 0x1122334455667788;
  body.signed_tx.tx.invocation.calldata = {0x00, 0x11, 0x22, 0x33, 0x44};
  body.signed_tx.tx.uniquifier.block_expiry_height = 987654321;
  body.signed_tx.tx.uniquifier.nonce = 0x8877665544332211;
  body.signed_tx.tx.auth_claim.source_address_claim().allowed_keys = 0xa5a5;
  body.signed_tx.tx.auth_claim.source_address_claim().required_weight = 9;
  body.nondeterministic_proposal_data.nondeterministic_bits = {0xa5, 0x03};
  body.nondeterministic_proposal_data.nondeterministic_bit_count = 10;
  body.nondeterministic_proposal_data.observed_queue_entry_sequence = {
      QueueEntry{0x11, 0x22}, QueueEntry{0x33}};
  payload.tx_bodies.push_back(std::move(body));

  payload.event_logs[0].tx_hash = sha256(payload.tx_bodies[0].signed_tx.tx);
  ProposalTxs proposal_txs = make_canonical_proposal_txs(payload.tx_bodies);
  value.proposal_header.events_hash =
      compute_indexer_outbox_event_root(payload.event_logs, payload.tx_bodies);
  auto txs_root = compute_indexer_proposal_txs_root(proposal_txs)();
  CHECK(txs_root.has_value());
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal_txs);
  return payload;
}

IndexerOutboxStore
open_store(const TmpDir &dir)
{
  auto store = IndexerOutboxStore::open(dir.directory());
  CHECK(store.has_value());
  return std::move(*store);
}

void
initialize_test_origin(IndexerOutboxStore &store,
                       const IndexerOutboxCommittedPayload &first)
{
  ASSERT_TRUE(store
                  .initialize_current_epoch_origin(
                      first.consensus_value.proposal_header.block_height,
                      first.consensus_value.prev_hash)()
                  .has_value());
}

std::string
path_for(const TmpDir &dir, std::string_view filename)
{
  return dir.directory() + "/" + std::string(filename);
}

x23::Buf
read_bytes(const TmpDir &dir, std::string_view filename)
{
  auto bytes = x23::readfile<x23::Buf>(path_for(dir, filename));
  CHECK(bytes.has_value());
  return std::move(*bytes);
}

void
write_bytes(const TmpDir &dir, std::string_view filename,
            std::span<const uint8_t> bytes)
{
  auto file =
      File::open(path_for(dir, filename), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(write_full(*file, std::as_bytes(bytes)).has_value());
  ASSERT_TRUE(file->close().has_value());
}

struct TestOutboxFile : IndexerOutboxRecordHeader {
  x23::Buf payload;
};

TestOutboxFile
read_envelope(const TmpDir &dir, std::string_view filename)
{
  TestOutboxFile envelope;
  x23::Buf bytes = read_bytes(dir, filename);
  uint32_t encoded_header_size = 0;
  x23::deserialize(&encoded_header_size, x23::BufView(bytes.data(), size_t{4}));
  IndexerOutboxRecordHeader header;
  x23::deserialize(&header,
                   x23::BufView(bytes.data() + 4, encoded_header_size));
  static_cast<IndexerOutboxRecordHeader &>(envelope) = std::move(header);
  envelope.payload =
      x23::Buf(bytes.begin() + 4 + encoded_header_size, bytes.end());
  return envelope;
}

void
write_envelope(const TmpDir &dir, std::string_view filename,
               const TestOutboxFile &envelope)
{
  x23::Buf header_bytes =
      x23::serialize(static_cast<const IndexerOutboxRecordHeader &>(envelope));
  x23::Buf bytes = x23::serialize(static_cast<uint32_t>(header_bytes.size()));
  bytes.append_range(header_bytes);
  bytes.append_range(envelope.payload);
  write_bytes(dir, filename, bytes);
}

void
write_sparse_record(const TmpDir &dir, std::string_view filename,
                    TestOutboxFile envelope, uint64_t payload_size)
{
  envelope.payload_byte_length = payload_size;
  x23::Buf header_bytes =
      x23::serialize(static_cast<const IndexerOutboxRecordHeader &>(envelope));
  x23::Buf prefix = x23::serialize(static_cast<uint32_t>(header_bytes.size()));
  prefix.append_range(header_bytes);
  auto file = File::open(path_for(dir, filename), O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(write_full(*file, std::as_bytes(std::span(prefix))).has_value());
  ASSERT_EQ(ftruncate(file->get_fd(),
                      // NOLINTNEXTLINE(misc-include-cleaner)
                      static_cast<off_t>(prefix.size() + payload_size)),
            0);
  ASSERT_TRUE(file->close().has_value());
}

x23::Buf
envelope_payload(const TestOutboxFile &envelope)
{
  return envelope.payload;
}

void
set_envelope_payload(TestOutboxFile &envelope, x23::Buf payload)
{
  envelope.payload = std::move(payload);
  envelope.payload_byte_length = envelope.payload.size();
  envelope.payload_digest = sha256(x23::BufView(envelope.payload));
}

template<typename Payload, typename Mutate>
void
mutate_payload(const TmpDir &dir, std::string_view filename, Mutate mutate)
{
  TestOutboxFile envelope = read_envelope(dir, filename);
  Payload payload;
  x23::Buf payload_bytes = envelope_payload(envelope);
  x23::deserialize(&payload, x23::BufView(payload_bytes));
  mutate(payload);
  set_envelope_payload(envelope, x23::serialize(payload));
  write_envelope(dir, filename, envelope);
}

template<typename Payload>
void
write_payload(const TmpDir &dir, std::string_view filename,
              const Payload &payload)
{
  TestOutboxFile envelope = read_envelope(dir, filename);
  set_envelope_payload(envelope, x23::serialize(payload));
  write_envelope(dir, filename, envelope);
}

template<typename T>
void
expect_xdr_equal(const T &lhs, const T &rhs)
{
  EXPECT_EQ(x23::serialize(lhs), x23::serialize(rhs));
}

class IndexerOutboxTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    std::filesystem::permissions(dir_.directory(),
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  TmpDir dir_;
};

TEST_F(IndexerOutboxTest, OpenRejectsMissingDirectory)
{
  auto result = IndexerOutboxStore::open(dir_.directory() + "/missing");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("existing"), std::string::npos);
}

TEST_F(IndexerOutboxTest, OpenOrCreateMakesPrivateDurableStoreDirectory)
{
  auto store = IndexerOutboxStore::open_or_create_in(dir_.directory());
  ASSERT_TRUE(store.has_value()) << store.error();
  const std::string path = dir_.directory() + "/indexer-outbox";
  struct stat stat_buf{};
  ASSERT_EQ(stat(path.c_str(), &stat_buf), 0);
  EXPECT_TRUE(S_ISDIR(stat_buf.st_mode));
  EXPECT_EQ(stat_buf.st_mode & 07777, 0700);
}

TEST_F(IndexerOutboxTest, OpenOrCreateClearsInheritedSetgidBit)
{
  const std::string path = dir_.directory() + "/indexer-outbox";
  ASSERT_TRUE(std::filesystem::create_directory(path));
  ASSERT_EQ(chown(path.c_str(), static_cast<uid_t>(-1), getegid()), 0);
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::set_gid,
                               std::filesystem::perm_options::replace);
  struct stat stat_buf{};
  ASSERT_EQ(stat(path.c_str(), &stat_buf), 0);
  ASSERT_EQ(stat_buf.st_mode & 07777, 02700);

  auto store = IndexerOutboxStore::open_or_create_in(dir_.directory());
  ASSERT_TRUE(store.has_value()) << store.error();
  ASSERT_EQ(stat(path.c_str(), &stat_buf), 0);
  EXPECT_EQ(stat_buf.st_mode & 07777, 0700);
}

TEST_F(IndexerOutboxTest, OpenOrCreateRetriesParentFsyncAfterExistingChild)
{
  fail_next_indexer_outbox_operation_for_test(
      IndexerOutboxFaultPoint::kFsyncParentDirectory);
  auto first = IndexerOutboxStore::open_or_create_in(dir_.directory());
  ASSERT_FALSE(first.has_value());
  EXPECT_TRUE(
      std::filesystem::is_directory(dir_.directory() + "/indexer-outbox"));

  fail_next_indexer_outbox_operation_for_test(
      IndexerOutboxFaultPoint::kFsyncParentDirectory);
  EXPECT_FALSE(
      IndexerOutboxStore::open_or_create_in(dir_.directory()).has_value());
  EXPECT_TRUE(
      IndexerOutboxStore::open_or_create_in(dir_.directory()).has_value());
}

TEST_F(IndexerOutboxTest, OpenOrCreateRejectsWrongExistingTypeAndPermissions)
{
  const std::string path = dir_.directory() + "/indexer-outbox";
  ASSERT_TRUE(File::open(path, O_CREAT | O_WRONLY, 0600).has_value());
  auto wrong_type = IndexerOutboxStore::open_or_create_in(dir_.directory());
  ASSERT_FALSE(wrong_type.has_value());
  ASSERT_TRUE(std::filesystem::remove(path));

  ASSERT_TRUE(std::filesystem::create_directory(path));
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read,
                               std::filesystem::perm_options::replace);
  auto wrong_permissions =
      IndexerOutboxStore::open_or_create_in(dir_.directory());
  ASSERT_FALSE(wrong_permissions.has_value());
  EXPECT_NE(wrong_permissions.error().find("0700"), std::string::npos);
}

TEST_F(IndexerOutboxTest, OpenCleansOnlyRecognizedRegularStaleTemps)
{
  const std::string stale =
      IndexerOutboxStore::candidate_filename(1) + ".tmp.123.456";
  const std::string unknown =
      IndexerOutboxStore::candidate_filename(2) + ".tmp.not-recognized";
  const std::string final = IndexerOutboxStore::candidate_filename(3);
  const std::vector<uint8_t> bytes = {0, 0, 0, 0};
  write_bytes(dir_, stale, bytes);
  write_bytes(dir_, unknown, bytes);
  write_bytes(dir_, final, bytes);

  auto store = IndexerOutboxStore::open(dir_.directory());

  ASSERT_TRUE(store.has_value()) << store.error();
  ASSERT_TRUE(store->recognized_namespace_empty().has_value());
  EXPECT_FALSE(*store->recognized_namespace_empty());
  EXPECT_FALSE(std::filesystem::exists(path_for(dir_, stale)));
  EXPECT_TRUE(std::filesystem::exists(path_for(dir_, unknown)));
  EXPECT_TRUE(std::filesystem::exists(path_for(dir_, final)));
}

TEST_F(IndexerOutboxTest, StoreOwnershipIsExclusiveUntilClose)
{
  {
    auto first = IndexerOutboxStore::open(dir_.directory());
    ASSERT_TRUE(first.has_value()) << first.error();
    auto second = IndexerOutboxStore::open(dir_.directory());
    ASSERT_FALSE(second.has_value());
    EXPECT_NE(second.error().find("locked"), std::string::npos);
  }

  auto later = IndexerOutboxStore::open(dir_.directory());
  ASSERT_TRUE(later.has_value()) << later.error();
}

TEST_F(IndexerOutboxTest, RecognizedNamespaceIncludesCandidatesAndOrigins)
{
  IndexerOutboxStore store = open_store(dir_);
  ASSERT_TRUE(store.recognized_namespace_empty().has_value());
  EXPECT_TRUE(*store.recognized_namespace_empty());
  ASSERT_TRUE(
      store.publish_candidate(make_candidate(700, 1, {}, {}))().has_value());
  EXPECT_FALSE(*store.recognized_namespace_empty());
  ASSERT_TRUE(store.remove_candidate(700).has_value());
  EXPECT_TRUE(*store.recognized_namespace_empty());
  ConsensusValueHash parent{};
  parent[0] = 1;
  ASSERT_TRUE(store.initialize_current_epoch_origin(1, parent)().has_value());
  EXPECT_FALSE(*store.recognized_namespace_empty());
}

TEST_F(IndexerOutboxTest, FirstCommittedRecordMustMatchPreparedEpochOrigin)
{
  ConsensusValueHash parent{};
  parent[0] = 0x42;
  const auto candidate = make_candidate(701, 1, {}, {});
  {
    IndexerOutboxStore store = open_store(dir_);
    ASSERT_TRUE(store.initialize_current_epoch_origin(1, parent)().has_value());
    ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
    auto mismatched = make_committed(1, {}, {});
    mismatched.consensus_value.prev_hash = parent;
    mismatched.consensus_value.prev_hash[0] ^= 0xff;

    auto published = store.publish_committed(mismatched)();
    ASSERT_FALSE(published.has_value());
    EXPECT_NE(published.error().find("epoch origin"), std::string::npos);
    EXPECT_FALSE(store.has_committed(1).value());
    EXPECT_TRUE(store.has_candidate(701).value());
  }

  IndexerOutboxStore reopened = open_store(dir_);
  EXPECT_FALSE(reopened.has_committed(1).value());
  EXPECT_TRUE(reopened.has_candidate(701).value());
  auto correct = make_committed(1, {}, {});
  correct.consensus_value.prev_hash = parent;
  ASSERT_TRUE(reopened.publish_committed(correct)().has_value());
  auto committed = reopened.load_committed(1)();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(committed->consensus_value.prev_hash, parent);
}

TEST_F(IndexerOutboxTest,
       CommittedPublicationWithoutOriginPreservesCandidateAndCreatesNothing)
{
  for (const BlockHeight height : {BlockHeight{1}, BlockHeight{37}}) {
    TmpDir dir;
    std::filesystem::permissions(dir.directory(),
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    IndexerOutboxStore store = open_store(dir);
    const auto block_id = static_cast<BlockID>(800 + height);
    ASSERT_TRUE(
        store.publish_candidate(make_candidate(block_id, height, {}, {}))()
            .has_value());
    auto committed = make_committed(height, {}, {});
    committed.consensus_value.prev_hash[0] = static_cast<uint8_t>(height);

    auto published = store.publish_committed(committed)();

    ASSERT_FALSE(published.has_value());
    EXPECT_NE(published.error().find("must be established"), std::string::npos);
    EXPECT_FALSE(store.has_committed(height).value());
    EXPECT_TRUE(store.has_candidate(block_id).value());
    auto origin = store.load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    EXPECT_FALSE(origin->has_value());
  }
}

TEST_F(IndexerOutboxTest, CommittedPublicationRequiresCanonicalNextRecord)
{
  IndexerOutboxStore store = open_store(dir_);
  ConsensusValueHash genesis_hash{};
  genesis_hash[0] = 0x51;
  ASSERT_TRUE(
      store.initialize_current_epoch_origin(1, genesis_hash)().has_value());

  auto height_two = make_committed(2, {}, {});
  height_two.consensus_value.prev_hash = genesis_hash;
  EXPECT_FALSE(store.publish_committed(height_two)().has_value());
  EXPECT_FALSE(store.has_committed(2).value());

  auto height_one = make_committed(1, {}, {});
  height_one.consensus_value.prev_hash = genesis_hash;
  ASSERT_TRUE(store.publish_committed(height_one)().has_value());
  auto height_three = make_committed(3, {}, {});
  height_three.consensus_value.prev_hash = sha256(height_one.consensus_value);
  EXPECT_FALSE(store.publish_committed(height_three)().has_value());
  EXPECT_FALSE(store.has_committed(3).value());

  auto height_two_wrong = make_committed(2, {}, {});
  height_two_wrong.consensus_value.prev_hash = genesis_hash;
  EXPECT_FALSE(store.publish_committed(height_two_wrong)().has_value());
  EXPECT_FALSE(store.has_committed(2).value());

  height_two.consensus_value.prev_hash = sha256(height_one.consensus_value);
  ASSERT_TRUE(store.publish_committed(height_two)().has_value());
  height_three.consensus_value.prev_hash = sha256(height_two.consensus_value);
  ASSERT_TRUE(store.publish_committed(height_three)().has_value());
  EXPECT_TRUE(store.publish_committed(height_one)().has_value());
}

TEST_F(IndexerOutboxTest, CommittedAppendScansHistoryOnceAndRebuildsOnReopen)
{
  ConsensusValueHash previous{};
  {
    IndexerOutboxStore store = open_store(dir_);
    EXPECT_EQ(store.committed_history_scan_count_for_test()(), 0UL);
    auto first = make_committed(1, {}, {});
    first.consensus_value.prev_hash = previous;
    initialize_test_origin(store, first);
    for (BlockHeight height = 1; height <= 64; ++height) {
      auto committed = make_committed(height, {}, {});
      committed.consensus_value.prev_hash = previous;
      ASSERT_TRUE(store.publish_committed(committed)().has_value());
      previous = sha256(committed.consensus_value);
      EXPECT_EQ(store.committed_history_scan_count_for_test()(), 1UL);
    }
  }

  IndexerOutboxStore reopened = open_store(dir_);
  EXPECT_EQ(reopened.committed_history_scan_count_for_test()(), 0UL);
  auto height_65 = make_committed(65, {}, {});
  height_65.consensus_value.prev_hash = previous;
  ASSERT_TRUE(reopened.publish_committed(height_65)().has_value());
  EXPECT_EQ(reopened.committed_history_scan_count_for_test()(), 1UL);

  auto height_66 = make_committed(66, {}, {});
  height_66.consensus_value.prev_hash = sha256(height_65.consensus_value);
  ASSERT_TRUE(reopened.publish_committed(height_66)().has_value());
  EXPECT_EQ(reopened.committed_history_scan_count_for_test()(), 1UL);
}

TEST_F(IndexerOutboxTest, ValidatedHistorySnapshotInstallsMatchingAppendCache)
{
  auto first = make_committed(1, {}, {});
  {
    IndexerOutboxStore store = open_store(dir_);
    initialize_test_origin(store, first);
    ASSERT_TRUE(store.publish_committed(first)().has_value());
  }

  IndexerOutboxStore reopened = open_store(dir_);
  auto history = reopened.validate_and_cache_committed_history_snapshot()();
  ASSERT_TRUE(history.has_value()) << history.error();
  ASSERT_EQ(history->records.size(), 1UL);
  ASSERT_TRUE(history->latest.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*history->latest, history->records.front());

  auto cached = reopened.lookup_validated_committed_record(1)();
  ASSERT_TRUE(cached.has_value()) << cached.error();
  ASSERT_TRUE(cached->has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(**cached, *history->latest);

  auto second = make_committed(2, {}, {});
  second.consensus_value.prev_hash = sha256(first.consensus_value);
  ASSERT_TRUE(reopened.publish_committed(second)().has_value());
  EXPECT_EQ(reopened.committed_history_scan_count_for_test()(), 1UL);
}

TEST_F(IndexerOutboxTest, PostLinkFailureRetryRepairsAppendCache)
{
  constexpr std::array kFaults{
      IndexerOutboxFaultPoint::kVerifyPublishedInode,
      IndexerOutboxFaultPoint::kClosePublishedTemp,
      IndexerOutboxFaultPoint::kUnlinkPublishedTemp,
      IndexerOutboxFaultPoint::kFsyncPublishedDirectory,
  };
  for (size_t i = 0; i < kFaults.size(); ++i) {
    TmpDir dir;
    std::filesystem::permissions(dir.directory(),
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    auto first = make_committed(1, {}, {});
    auto second = make_committed(2, {}, {});
    second.consensus_value.prev_hash = sha256(first.consensus_value);
    auto third = make_committed(3, {}, {});
    third.consensus_value.prev_hash = sha256(second.consensus_value);
    {
      IndexerOutboxStore store = open_store(dir);
      ASSERT_TRUE(store.publish_candidate(make_candidate(900 + i, 1, {}, {}))()
                      .has_value());
      ASSERT_TRUE(store
                      .initialize_current_epoch_origin(
                          1, first.consensus_value.prev_hash)()
                      .has_value());
      fail_next_indexer_outbox_operation_for_test(kFaults[i]);
      EXPECT_FALSE(store.publish_committed(first)().has_value());
      EXPECT_TRUE(store.has_committed(1).value());
      EXPECT_TRUE(store.publish_committed(first)().has_value());
      EXPECT_TRUE(store.publish_committed(second)().has_value());
      EXPECT_TRUE(store.publish_committed(first)().has_value());
      EXPECT_TRUE(store.publish_committed(third)().has_value());
      EXPECT_TRUE(store.has_candidate(900 + i).value());
    }
    IndexerOutboxStore reopened = open_store(dir);
    EXPECT_TRUE(reopened.load_committed(3)().has_value());
    EXPECT_TRUE(reopened.has_candidate(900 + i).value());
  }
}

TEST_F(IndexerOutboxTest, ExistingOriginRetryRepairsDirectoryDurability)
{
  constexpr std::array kFaults{
      IndexerOutboxFaultPoint::kVerifyPublishedInode,
      IndexerOutboxFaultPoint::kClosePublishedTemp,
      IndexerOutboxFaultPoint::kUnlinkPublishedTemp,
      IndexerOutboxFaultPoint::kFsyncPublishedDirectory,
  };
  for (const auto fault : kFaults) {
    TmpDir dir;
    std::filesystem::permissions(dir.directory(),
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    ConsensusValueHash boundary{};
    boundary[0] = 0x61;
    {
      IndexerOutboxStore store = open_store(dir);
      fail_next_indexer_outbox_operation_for_test(fault);
      EXPECT_FALSE(
          store.initialize_current_epoch_origin(1, boundary)().has_value());
      ASSERT_TRUE(store.load_current_epoch_origin()->has_value());

      fail_next_indexer_outbox_operation_for_test(
          IndexerOutboxFaultPoint::kFsyncPublishedDirectory);
      EXPECT_FALSE(
          store.initialize_current_epoch_origin(1, boundary)().has_value());
      EXPECT_TRUE(
          store.initialize_current_epoch_origin(1, boundary)().has_value());
    }

    IndexerOutboxStore reopened = open_store(dir);
    auto origin = reopened.load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    ASSERT_TRUE(origin->has_value());
    const auto &origin_value = origin.value();
    const auto *current_origin =
        origin_value.has_value() ? &origin_value.value() : nullptr;
    ASSERT_NE(current_origin, nullptr);
    EXPECT_EQ(current_origin->origin.first_height, 1);
    EXPECT_EQ(current_origin->origin.first_prev_hash, boundary);
  }
}

TEST_F(IndexerOutboxTest, ExactExistingCandidateRetryFsyncsDirectory)
{
  IndexerOutboxStore store = open_store(dir_);
  const auto candidate = make_candidate(901, 1, {}, {});
  fail_next_indexer_outbox_operation_for_test(
      IndexerOutboxFaultPoint::kClosePublishedTemp);
  EXPECT_FALSE(store.publish_candidate(candidate)().has_value());
  ASSERT_TRUE(store.has_candidate(901).value());

  fail_next_indexer_outbox_operation_for_test(
      IndexerOutboxFaultPoint::kFsyncPublishedDirectory);
  EXPECT_FALSE(store.publish_candidate(candidate)().has_value());
  EXPECT_TRUE(store.publish_candidate(candidate)().has_value());
}

TEST_F(IndexerOutboxTest, ExactExistingCommittedRetryFsyncsDirectory)
{
  IndexerOutboxStore store = open_store(dir_);
  const auto committed = make_committed(1, {}, {});
  ASSERT_TRUE(store
                  .initialize_current_epoch_origin(
                      1, committed.consensus_value.prev_hash)()
                  .has_value());
  fail_next_indexer_outbox_operation_for_test(
      IndexerOutboxFaultPoint::kClosePublishedTemp);
  EXPECT_FALSE(store.publish_committed(committed)().has_value());
  ASSERT_TRUE(store.has_committed(1).value());

  fail_next_indexer_outbox_operation_for_test(
      IndexerOutboxFaultPoint::kFsyncPublishedDirectory);
  EXPECT_FALSE(store.publish_committed(committed)().has_value());
  EXPECT_TRUE(store.publish_committed(committed)().has_value());
}

TEST_F(IndexerOutboxTest, EpochOriginXdrMatchesCrossLanguageGolden)
{
  IndexerOutboxOrigin origin{
      .protocol = "hogp/archive_a",
      .epoch_sequence = 0x0102030405060708,
      .first_height = 123456789,
  };
  origin.first_prev_hash[0] = 0x10;
  origin.first_prev_hash[31] = 0x2f;
  const x23::Buf encoded = x23::serialize(origin);

  constexpr size_t kExpectedByteLength = 68;
  const SHA256Hash kExpectedDigest{0x33, 0x1f, 0xa7, 0xd2, 0x5a, 0x47, 0x42,
                                   0x29, 0x8e, 0x93, 0xbe, 0xab, 0x33, 0xc7,
                                   0x33, 0x45, 0xe2, 0x36, 0xb7, 0x52, 0x98,
                                   0x78, 0xda, 0x8d, 0x1a, 0x33, 0x27, 0xbf,
                                   0x8e, 0xaf, 0x52, 0xbd};
  EXPECT_EQ(encoded.size(), kExpectedByteLength);
  EXPECT_EQ(sha256(encoded), kExpectedDigest);
}

TEST_F(IndexerOutboxTest, ConcurrentCommittedPublicationCannotForkOrSkip)
{
  IndexerOutboxStore store = open_store(dir_);
  ConsensusValueHash genesis_hash{};
  genesis_hash[0] = 0x52;
  ASSERT_TRUE(
      store.initialize_current_epoch_origin(1, genesis_hash)().has_value());
  auto height_one = make_committed(1, {}, {});
  height_one.consensus_value.prev_hash = genesis_hash;
  ASSERT_TRUE(store.publish_committed(height_one)().has_value());

  auto first_height_two = make_committed(2, {}, {});
  first_height_two.consensus_value.prev_hash =
      sha256(height_one.consensus_value);
  auto competing_height_two = first_height_two;
  competing_height_two.consensus_value.proposal_header.block_timepoint++;
  std::latch start_fork(1);
  auto first = std::async(std::launch::async, [&] {
    start_fork.wait();
    return store.publish_committed(first_height_two)();
  });
  auto competing = std::async(std::launch::async, [&] {
    start_fork.wait();
    return store.publish_committed(competing_height_two)();
  });
  start_fork.count_down();
  const bool first_published = first.get().has_value();
  const bool competing_published = competing.get().has_value();
  EXPECT_NE(first_published, competing_published);
  auto committed_two = store.load_committed(2)();
  ASSERT_TRUE(committed_two.has_value()) << committed_two.error();

  auto height_three = make_committed(3, {}, {});
  height_three.consensus_value.prev_hash =
      sha256(committed_two->consensus_value);
  auto height_four = make_committed(4, {}, {});
  height_four.consensus_value.prev_hash = sha256(height_three.consensus_value);
  std::latch start_gap(1);
  auto next = std::async(std::launch::async, [&] {
    start_gap.wait();
    return store.publish_committed(height_three)();
  });
  auto skipped = std::async(std::launch::async, [&] {
    start_gap.wait();
    return store.publish_committed(height_four)();
  });
  start_gap.count_down();
  const bool next_published = next.get().has_value();
  const bool skipped_published = skipped.get().has_value();
  EXPECT_TRUE(next_published);
  EXPECT_FALSE(skipped_published && !store.has_committed(3).value());
  if (!skipped_published) {
    ASSERT_TRUE(store.publish_committed(height_four)().has_value());
  }
  EXPECT_TRUE(store.has_committed(3).value());
  EXPECT_TRUE(store.has_committed(4).value());
}

TEST_F(IndexerOutboxTest, NonEmptyCandidateAndCommittedRoundTrip)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction body_a = make_body(1);
  ProposedTransaction body_b = make_body(2);
  ProposedTransaction body_c = make_body(3);
  ProposedTransactionBatch proposal_bodies{body_a, body_b, body_c};
  ProposalTxs proposal_txs = make_canonical_proposal_txs(proposal_bodies);
  ASSERT_GE(proposal_txs.hash.segment_tokens.size(), 2);
  ASSERT_GE(proposal_txs.nondeterministic_data.segment_tokens.size(), 2);

  ProposedTransactionBatch arrival_bodies{body_a, body_b, body_c};
  TxEventLogBatch arrival_events{make_event(body_a, 0x11),
                                 make_event(body_b, 0x12),
                                 make_event(body_c, 0x13)};
  auto candidate = make_candidate(42, 7, arrival_bodies, arrival_events);
  auto committed = make_committed_with_proposal(7, proposal_txs, arrival_bodies,
                                                arrival_events);
  committed.consensus_value.prev_hash[0] = 0x77;
  EXPECT_NE(committed.consensus_value.proposal_txs_hash, ProposalTxsHash{});

  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  auto loaded_candidate = store.load_candidate(42)();
  auto loaded_committed = store.load_committed(7)();
  auto epoch_origin = store.load_current_epoch_origin();

  ASSERT_TRUE(loaded_candidate.has_value()) << loaded_candidate.error();
  ASSERT_TRUE(loaded_committed.has_value()) << loaded_committed.error();
  expect_xdr_equal(*loaded_candidate, candidate);
  expect_xdr_equal(*loaded_committed, committed);
  ASSERT_TRUE(epoch_origin.has_value()) << epoch_origin.error();
  ASSERT_TRUE(epoch_origin->has_value());
  const auto &origin_value = epoch_origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);
  EXPECT_EQ(current_origin->origin.first_height, 7);
  EXPECT_EQ(current_origin->origin.first_prev_hash,
            committed.consensus_value.prev_hash);
  EXPECT_EQ(std::string_view(current_origin->origin.protocol),
            version::kHogdProtocol());
  EXPECT_TRUE(store
                  .initialize_current_epoch_origin(
                      7, committed.consensus_value.prev_hash)()
                  .has_value());
  EXPECT_FALSE(store
                   .initialize_current_epoch_origin(
                       8, committed.consensus_value.prev_hash)()
                   .has_value());
}

TEST_F(IndexerOutboxTest, DurableSemanticValidationAwaitsOnPoolWorker)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction body_a = make_body(101);
  ProposedTransaction body_b = make_body(102);
  TxEventLogBatch events{make_event(body_a, 0xa1), make_event(body_b, 0xa2)};
  IndexerOutboxCommittedPayload committed =
      make_committed(22, {body_a, body_b}, events);
  IndexerOutboxCandidatePayload candidate =
      make_candidate(220, 22, {body_a, body_b}, std::move(events));

  auto exercise = [&]() -> x23::Async<bool> {
    co_await default_threadpool().bg();
    if (!(co_await store.publish_candidate(candidate)) ||
        !(co_await store.initialize_current_epoch_origin(
            committed.consensus_value.proposal_header.block_height,
            committed.consensus_value.prev_hash)) ||
        !(co_await store.publish_committed(committed))) {
      co_return false;
    }
    auto loaded_candidate = co_await store.load_candidate(candidate.block_id);
    auto loaded_committed = co_await store.load_committed(
        committed.consensus_value.proposal_header.block_height);
    if (!loaded_candidate || !loaded_committed) {
      co_return false;
    }
    auto block = co_await make_indexer_committed_block(*loaded_committed);
    co_return block.has_value() &&
        (co_await validate_indexer_committed_block(*block)).has_value();
  };

  EXPECT_TRUE(exercise()());
}

TEST_F(IndexerOutboxTest, EventlessTransactionBodyRoundTrip)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction body = make_body(2);
  TxEventLog event = make_event(body, 0x22, /*with_emitted_event=*/false);
  auto committed = make_committed(8, {body}, {event});

  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  auto loaded = store.load_committed(8)();

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_EQ(loaded->event_logs.size(), 1);
  EXPECT_TRUE(loaded->event_logs[0].events.empty());
  expect_xdr_equal(loaded->tx_bodies[0], body);
}

TEST_F(IndexerOutboxTest, EmptyBlocksRoundTrip)
{
  IndexerOutboxStore store = open_store(dir_);
  auto candidate = make_candidate(43, 9, {}, {});
  auto committed = make_committed(9, {}, {});

  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  auto loaded_candidate = store.load_candidate(43)();
  auto loaded_committed = store.load_committed(9)();

  ASSERT_TRUE(loaded_candidate.has_value()) << loaded_candidate.error();
  ASSERT_TRUE(loaded_committed.has_value()) << loaded_committed.error();
  EXPECT_TRUE(loaded_candidate->event_logs.empty());
  EXPECT_TRUE(loaded_candidate->tx_bodies.empty());
  EXPECT_TRUE(loaded_committed->event_logs.empty());
  EXPECT_TRUE(loaded_committed->tx_bodies.empty());
}

TEST_F(IndexerOutboxTest, WireBlockUsesExactDurablePayloadDigest)
{
  IndexerOutboxStore store = open_store(dir_);
  IndexerOutboxCommittedPayload committed = make_committed(17, {}, {});
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  auto loaded = store.load_committed(17)();
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  auto wire = make_indexer_committed_block(*loaded)();
  ASSERT_TRUE(wire.has_value()) << wire.error();
  EXPECT_EQ(x23::serialize(wire->payload), x23::serialize(*loaded));
  EXPECT_EQ(wire->identity.payload_digest, sha256(*loaded));
  EXPECT_EQ(wire->identity.consensus_hash, sha256(loaded->consensus_value));
  EXPECT_TRUE(validate_indexer_committed_block(*wire)().has_value());
}

TEST_F(IndexerOutboxTest, CommittedPayloadXdrMatchesCrossLanguageGolden)
{
  IndexerCommittedBlockPayload payload =
      make_committed_payload_golden_fixture();
  ASSERT_TRUE(validate_indexer_committed_payload(payload)().has_value());
  const x23::Buf encoded = x23::serialize(payload);
  constexpr size_t kExpectedByteLength = 576;
  const SHA256Hash kExpectedDigest{0xac, 0x20, 0x4b, 0x69, 0x7f, 0x4a, 0x80,
                                   0xfb, 0x7c, 0x64, 0x1d, 0x1f, 0x2b, 0x13,
                                   0xe8, 0xc8, 0x43, 0xdf, 0x11, 0x77, 0xa9,
                                   0xbd, 0x06, 0x0b, 0xbf, 0x44, 0x47, 0xd6,
                                   0x4d, 0xf2, 0x6e, 0xd6};

  EXPECT_EQ(encoded.size(), kExpectedByteLength);
  EXPECT_EQ(sha256(encoded), kExpectedDigest);
}

TEST_F(IndexerOutboxTest, StableEnvelopeReservedSlotPreservesGolden)
{
  IndexerOutboxRecordHeader header{
      .format_version = kIndexerOutboxEnvelopeFormatVersion,
      .epoch_sequence = 0x0102030405060708,
      .block_height = 123456789,
      .protocol = "hogp/archive_a",
      .payload_byte_length = 0x1112131415161718,
  };
  header.record_identity.kind(IndexerOutboxRecordKind::IndexerOutboxCommitted);
  header.payload_digest[0] = 0x21;
  header.payload_digest[31] = 0x40;
  header.consensus_hash[0] = 0x41;
  header.consensus_hash[31] = 0x60;
  header.prev_hash[0] = 0x61;
  header.prev_hash[31] = 0x80;
  // This zero-valued reserved slot occupied the same wire position as the old
  // ProposalTxsHash field. Keeping the existing golden pins header decoding
  // compatibility across the semantic rename.
  header.reserved_hash = {};
  const x23::Buf encoded = x23::serialize(header);

  constexpr size_t kExpectedByteLength = 212;
  const SHA256Hash kExpectedDigest{0x34, 0x06, 0x07, 0xe7, 0x3b, 0xd3, 0xa4,
                                   0xcf, 0xb0, 0x2f, 0x43, 0xc3, 0x80, 0x3c,
                                   0xa0, 0x44, 0x96, 0x2f, 0xd2, 0xa8, 0xd4,
                                   0x53, 0x12, 0x07, 0x56, 0xc5, 0x67, 0x82,
                                   0xf6, 0x24, 0xd8, 0x68};
  EXPECT_EQ(encoded.size(), kExpectedByteLength);
  EXPECT_EQ(sha256(encoded), kExpectedDigest);
}

TEST_F(IndexerOutboxTest, ByteIdenticalRepublishIsIdempotent)
{
  IndexerOutboxStore store = open_store(dir_);
  auto committed = make_committed(10, {}, {});

  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  x23::Buf before =
      read_bytes(dir_, IndexerOutboxStore::committed_filename(10));
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  x23::Buf after = read_bytes(dir_, IndexerOutboxStore::committed_filename(10));

  EXPECT_EQ(before, after);
}

TEST_F(IndexerOutboxTest, ConflictingRepublishIsRejected)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction first_body = make_body(3);
  ProposedTransaction second_body = make_body(4);
  auto first =
      make_candidate(44, 11, {first_body}, {make_event(first_body, 0x33)});
  auto second =
      make_candidate(44, 11, {second_body}, {make_event(second_body, 0x44)});

  ASSERT_TRUE(store.publish_candidate(first)().has_value());
  auto result = store.publish_candidate(second)();

  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("mismatched"), std::string::npos);
}

TEST_F(IndexerOutboxTest, EncodedCandidateMatchesStructuredByteForByte)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction body_a = make_body(1);
  ProposedTransaction body_b = make_body(2);
  ProposedTransaction body_c = make_body(3);
  ProposedTransactionBatch bodies{body_a, body_b, body_c};
  TxEventLogBatch events{make_event(body_a, 0x21), make_event(body_b, 0x22),
                         make_event(body_c, 0x23)};
  const auto candidate = make_candidate(42, 7, bodies, events);

  // Encode the payload's two vectors the way add_event() does: one buffer per
  // ingest thread, appended in lockstep. Splitting contiguously keeps each
  // concatenation in the payload's own element order.
  std::vector<x23::Buf> event_bufs(2);
  std::vector<x23::Buf> body_bufs(2);
  const size_t split = candidate.event_logs.size() / 2;
  for (size_t i = 0; i < candidate.event_logs.size(); ++i) {
    const size_t slot = i < split ? 0 : 1;
    x23::serialize_to(&event_bufs[slot], candidate.event_logs[i]);
    x23::serialize_to(&body_bufs[slot], candidate.tx_bodies[i]);
  }

  auto encoded = store.publish_candidate_encoded(
      42, 7, candidate.event_root,
      static_cast<uint32_t>(candidate.event_logs.size()), event_bufs,
      body_bufs);
  ASSERT_TRUE(encoded.has_value()) << encoded.error();

  // publish_bytes() replaces an existing record only when it is byte-identical,
  // so a structured publish succeeding on top of the encoded one is the
  // equivalence assertion: closing a block may skip re-encoding the payload
  // only for as long as these two agree exactly.
  auto structured = store.publish_candidate(candidate)();
  ASSERT_TRUE(structured.has_value()) << structured.error();

  auto loaded = store.load_candidate(42)();
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  expect_xdr_equal(*loaded, candidate);
}

TEST_F(IndexerOutboxTest, EncodedEmptyCandidateMatchesStructuredByteForByte)
{
  IndexerOutboxStore store = open_store(dir_);
  const auto candidate = make_candidate(43, 9, {}, {});

  auto encoded =
      store.publish_candidate_encoded(43, 9, candidate.event_root, 0, {}, {});
  ASSERT_TRUE(encoded.has_value()) << encoded.error();
  auto structured = store.publish_candidate(candidate)();
  ASSERT_TRUE(structured.has_value()) << structured.error();

  auto loaded = store.load_candidate(43)();
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  expect_xdr_equal(*loaded, candidate);
}

TEST_F(IndexerOutboxTest, RemoveCandidateIsDurableAndIdempotent)
{
  IndexerOutboxStore store = open_store(dir_);
  ASSERT_TRUE(
      store.publish_candidate(make_candidate(441, 11, {}, {}))().has_value());

  ASSERT_TRUE(store.remove_candidate(441).has_value());
  EXPECT_FALSE(store.load_candidate(441)().has_value());
  EXPECT_TRUE(store.remove_candidate(441).has_value());
}

TEST_F(IndexerOutboxTest, CorruptPayloadDigestIsRejected)
{
  // Committed records are archival, so they keep a payload digest and a
  // corrupted one is fatal on load.
  IndexerOutboxStore store = open_store(dir_);
  auto committed = make_committed(12, {}, {});
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  const std::string filename = IndexerOutboxStore::committed_filename(12);
  TestOutboxFile envelope = read_envelope(dir_, filename);
  envelope.payload_digest[0] ^= 0x01;
  write_envelope(dir_, filename, envelope);

  auto result = store.load_committed(12)();

  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("digest mismatch"), std::string::npos);
}

TEST_F(IndexerOutboxTest, CandidateCarriesNoPayloadDigest)
{
  // Candidates deliberately carry no payload digest. Promotion selects one by
  // the finalized events root and then re-derives that root and every body's
  // transaction hash, so hashing the payload at close -- and again on every
  // candidate enumeration, on the commit path -- bought no detection this path
  // did not already have. Pin both halves: the published digest is zero, and a
  // garbage digest does not fail the load.
  IndexerOutboxStore store = open_store(dir_);
  auto candidate = make_candidate(45, 12, {}, {});
  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  const std::string filename = IndexerOutboxStore::candidate_filename(45);
  TestOutboxFile envelope = read_envelope(dir_, filename);
  EXPECT_EQ(envelope.payload_digest, SHA256Hash{});
  envelope.payload_digest[0] ^= 0x01;
  write_envelope(dir_, filename, envelope);

  auto result = store.load_candidate(45)();

  ASSERT_TRUE(result.has_value()) << result.error();
  expect_xdr_equal(*result, candidate);
}

TEST_F(IndexerOutboxTest, EnvelopeAndPayloadVersionsAreRejected)
{
  IndexerOutboxStore store = open_store(dir_);
  auto candidate = make_candidate(46, 13, {}, {});
  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  const std::string filename = IndexerOutboxStore::candidate_filename(46);
  TestOutboxFile envelope = read_envelope(dir_, filename);
  envelope.format_version = kIndexerOutboxEnvelopeFormatVersion + 1;
  write_envelope(dir_, filename, envelope);
  auto envelope_result = store.load_candidate(46)();
  ASSERT_FALSE(envelope_result.has_value());
  EXPECT_NE(envelope_result.error().find("header version"), std::string::npos);

  envelope.format_version = kIndexerOutboxEnvelopeFormatVersion;
  write_envelope(dir_, filename, envelope);
  mutate_payload<IndexerOutboxCandidatePayload>(dir_, filename,
                                                [](auto &payload) {
                                                  payload.format_version++;
                                                });
  auto payload_result = store.load_candidate(46)();
  ASSERT_FALSE(payload_result.has_value());
  EXPECT_NE(payload_result.error().find("candidate outbox format"),
            std::string::npos);
}

TEST_F(IndexerOutboxTest, ProtocolMismatchPrecedesPayloadDecode)
{
  IndexerOutboxStore store = open_store(dir_);
  auto candidate = make_candidate(47, 14, {}, {});
  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  const std::string filename = IndexerOutboxStore::candidate_filename(47);
  TestOutboxFile envelope = read_envelope(dir_, filename);
  envelope.protocol = "hogp/different";
  set_envelope_payload(envelope, x23::Buf{0, 0, 0, 0});
  write_envelope(dir_, filename, envelope);

  auto result = store.load_candidate(47)();

  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("protocol"), std::string::npos);
}

TEST_F(IndexerOutboxTest, EnumerationKeepsOlderProtocolPayloadOpaque)
{
  IndexerOutboxStore store = open_store(dir_);
  auto committed = make_committed(15, {}, {});
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  const std::string filename = IndexerOutboxStore::committed_filename(15);
  TestOutboxFile envelope = read_envelope(dir_, filename);
  envelope.protocol = "hogp/archival";
  set_envelope_payload(envelope, x23::Buf{0xff, 0xff, 0xff, 0xff});
  write_envelope(dir_, filename, envelope);

  auto records = store.enumerate_committed_records_raw();
  ASSERT_TRUE(records.has_value()) << records.error();
  ASSERT_EQ(records->size(), 1);
  EXPECT_EQ(records->front().block_height, 15);
  EXPECT_EQ(records->front().protocol, "hogp/archival");
  EXPECT_FALSE(store.load_committed(15)().has_value());
}

TEST_F(IndexerOutboxTest, EnumerationKeepsOlderEpochOriginPayloadOpaque)
{
  IndexerOutboxStore store = open_store(dir_);
  auto committed = make_committed(15, {}, {});
  committed.consensus_value.prev_hash[0] = 0x42;
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  TestOutboxFile origin =
      read_envelope(dir_, IndexerOutboxStore::current_epoch_origin_filename());
  origin.protocol = "hogp/archival";
  set_envelope_payload(origin, x23::Buf{0xff, 0xff, 0xff, 0xff});
  write_envelope(dir_,
                 IndexerOutboxStore::epoch_origin_filename("hogp/archival"),
                 origin);

  auto origins = store.enumerate_epoch_origins_raw();
  ASSERT_TRUE(origins.has_value()) << origins.error();
  ASSERT_EQ(origins->size(), 2);
  auto archival = std::ranges::find(*origins, "hogp/archival",
                                    &IndexerOutboxEpochOriginInfo::protocol);
  ASSERT_NE(archival, origins->end());
  EXPECT_EQ(archival->first_height, 15);
  EXPECT_EQ(archival->first_prev_hash, committed.consensus_value.prev_hash);
}

TEST_F(IndexerOutboxTest, EnumerationRejectsSparseRecordWithInvalidDigest)
{
  IndexerOutboxStore store = open_store(dir_);
  auto committed = make_committed(151, {}, {});
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  const std::string filename = IndexerOutboxStore::committed_filename(151);
  TestOutboxFile record = read_envelope(dir_, filename);
  record.protocol = "hogp/archival";
  constexpr uint64_t kSparsePayloadBytes = 512ULL * 1024 * 1024;
  static_assert(kSparsePayloadBytes < kMaxIndexerOutboxPayloadBytes);
  write_sparse_record(dir_, filename, std::move(record), kSparsePayloadBytes);

  auto records = store.enumerate_committed_records_raw();
  ASSERT_FALSE(records.has_value());
  EXPECT_NE(records.error().find("digest mismatch"), std::string::npos);
}

TEST_F(IndexerOutboxTest,
       CandidateCardinalityAndCommittedBodyMismatchAreRejected)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction body = make_body(5);
  auto candidate = make_candidate(48, 15, {body}, {make_event(body, 0x55)});
  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  const std::string candidate_filename =
      IndexerOutboxStore::candidate_filename(48);
  mutate_payload<IndexerOutboxCandidatePayload>(dir_, candidate_filename,
                                                [](auto &payload) {
                                                  payload.tx_bodies.clear();
                                                });
  auto batch_mismatch = store.load_candidate(48)();
  ASSERT_FALSE(batch_mismatch.has_value());
  EXPECT_NE(batch_mismatch.error().find("cardinalities differ"),
            std::string::npos);

  auto committed =
      make_committed(16, {body}, {make_event(body, /*marker=*/0x66)});
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  const std::string committed_filename =
      IndexerOutboxStore::committed_filename(16);
  mutate_payload<IndexerOutboxCommittedPayload>(dir_, committed_filename,
                                                [](auto &payload) {
                                                  payload.tx_bodies[0] =
                                                      make_body(6);
                                                });

  auto body_mismatch = store.load_committed(16)();
  ASSERT_FALSE(body_mismatch.has_value());
  EXPECT_NE(body_mismatch.error().find("body hash"), std::string::npos);
}

TEST_F(IndexerOutboxTest,
       CommittedNondeterministicDataMutationIsRejectedByEventRoot)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction body = make_body(7);
  auto committed =
      make_committed(161, {body}, {make_event(body, /*marker=*/0x77)});
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  const std::string filename = IndexerOutboxStore::committed_filename(161);
  mutate_payload<IndexerOutboxCommittedPayload>(
      dir_, filename, [](auto &payload) {
        payload.tx_bodies[0]
            .nondeterministic_proposal_data.nondeterministic_bits[0] ^= 0xff;
      });

  auto result = store.load_committed(161)();

  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("event root"), std::string::npos);
}

TEST_F(IndexerOutboxTest, CandidateHeaderPayloadEventRootMismatchIsRejected)
{
  IndexerOutboxStore store = open_store(dir_);
  ProposedTransaction body_a = make_body(81);
  ProposedTransaction body_b = make_body(82);
  ProposedTransaction body_c = make_body(83);
  auto candidate =
      make_candidate(481, 162, {body_a, body_b, body_c},
                     {make_event(body_a, 0x81), make_event(body_b, 0x82),
                      make_event(body_c, 0x83)});
  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  const std::string filename = IndexerOutboxStore::candidate_filename(481);

  IndexerOutboxCandidatePayload mutated = candidate;
  mutated.event_root[0] ^= 1;
  write_payload(dir_, filename, mutated);
  auto result = store.load_candidate(481)();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("metadata mismatch"), std::string::npos);
}

TEST_F(IndexerOutboxTest, MalformedAndTrailingXdrAreRejected)
{
  IndexerOutboxStore store = open_store(dir_);
  const std::string malformed_name = IndexerOutboxStore::candidate_filename(49);
  const std::vector<uint8_t> malformed = {0, 0, 0, 1};
  write_bytes(dir_, malformed_name, malformed);
  auto malformed_result = store.load_candidate(49)();
  ASSERT_FALSE(malformed_result.has_value());
  EXPECT_NE(malformed_result.error().find("header"), std::string::npos);

  auto candidate = make_candidate(50, 17, {}, {});
  ASSERT_TRUE(store.publish_candidate(candidate)().has_value());
  const std::string trailing_name = IndexerOutboxStore::candidate_filename(50);
  x23::Buf trailing = read_bytes(dir_, trailing_name);
  trailing.append_range(std::vector<uint8_t>{0, 0, 0, 0});
  write_bytes(dir_, trailing_name, trailing);
  auto trailing_result = store.load_candidate(50)();
  ASSERT_FALSE(trailing_result.has_value());
  EXPECT_NE(trailing_result.error().find("size mismatch"), std::string::npos);
}

TEST_F(IndexerOutboxTest, CandidateAndCommittedIdentityMismatchesAreRejected)
{
  IndexerOutboxStore store = open_store(dir_);
  auto negative_candidate = make_candidate(51, -1, {}, {});
  auto candidate_result = store.publish_candidate(negative_candidate)();
  ASSERT_FALSE(candidate_result.has_value());
  EXPECT_NE(candidate_result.error().find("negative"), std::string::npos);

  auto committed = make_committed(18, {}, {});
  initialize_test_origin(store, committed);
  ASSERT_TRUE(store.publish_committed(committed)().has_value());
  const std::string filename = IndexerOutboxStore::committed_filename(18);
  mutate_payload<IndexerOutboxCommittedPayload>(
      dir_, filename, [](auto &payload) {
        payload.consensus_value.proposal_header.events_hash[0] ^= 0x01;
      });
  auto root_mismatch = store.load_committed(18)();
  ASSERT_FALSE(root_mismatch.has_value());
  EXPECT_NE(root_mismatch.error().find("event root"), std::string::npos);

  mutate_payload<IndexerOutboxCommittedPayload>(
      dir_, filename, [](auto &payload) {
        payload.consensus_value.proposal_header.events_hash =
            compute_indexer_outbox_event_root(payload.event_logs,
                                              payload.tx_bodies);
        payload.consensus_value.proposal_header.block_height = 19;
      });
  auto height_mismatch = store.load_committed(18)();
  ASSERT_FALSE(height_mismatch.has_value());
  EXPECT_NE(height_mismatch.error().find("filename height"), std::string::npos);

  mutate_payload<IndexerOutboxCommittedPayload>(
      dir_, filename, [](auto &payload) {
        payload.consensus_value.proposal_header.block_height = -1;
      });
  auto negative_height = store.load_committed(18)();
  ASSERT_FALSE(negative_height.has_value());
  EXPECT_NE(negative_height.error().find("negative"), std::string::npos);
}

TEST_F(IndexerOutboxTest,
       PullRpcSizePreflightCountsEveryPayloadPartAtExactBoundary)
{
  static_assert(kIndexerRpcMaxMessageSize == 0x7fffffff);
  static_assert(kEventSchedulerRpcMaxMessageSize == kIndexerRpcMaxMessageSize);
  ProposedTransaction body = make_body(1);
  IndexerOutboxCommittedPayload payload =
      make_committed(1, {body}, {make_event(body, 1)});
  IndexerCommittedBlock block = *make_indexer_committed_block(payload)();
  IndexerOutboxOrigin origin{.protocol = version::kHogdProtocol(),
                             .first_height = 1};
  EventIndexerPullResult result(EventIndexerPullOutcome::EventIndexerPullBlock);
  result.pulled_block() = {
      .origin = std::move(origin),
      .block = std::move(block),
  };
  x23::rpc_msg reply{};
  x23::msg_set_reply_stat(&reply, x23::accept_stat::SUCCESS);
  const x23::Buf actual_record = x23::make_record(reply, result);
  auto measured = indexer_event_pull_rpc_record_size_for_parts(
      x23::xdr_size(payload.consensus_value), x23::xdr_size(payload.event_logs),
      x23::xdr_size(payload.tx_bodies));
  ASSERT_TRUE(measured.has_value()) << measured.error();
  EXPECT_EQ(*measured, actual_record.size());

  const size_t fixed = *indexer_event_pull_rpc_record_size_for_parts(0, 0, 0);
  const size_t large_event_bytes = kMaxIndexerEventPullRpcRecordBytes - fixed;
  auto max_minus_one =
      indexer_event_pull_rpc_record_size_for_parts(0, large_event_bytes - 1, 0);
  auto at_max =
      indexer_event_pull_rpc_record_size_for_parts(0, large_event_bytes, 0);
  auto overflow =
      indexer_event_pull_rpc_record_size_for_parts(0, large_event_bytes + 1, 0);
  ASSERT_TRUE(max_minus_one.has_value()) << max_minus_one.error();
  ASSERT_TRUE(at_max.has_value()) << at_max.error();
  EXPECT_EQ(*max_minus_one, kMaxIndexerEventPullRpcRecordBytes - 1);
  EXPECT_EQ(*at_max, kMaxIndexerEventPullRpcRecordBytes);
  EXPECT_FALSE(overflow.has_value());

  auto at_limit =
      indexer_outbox_file_size_for_payload(kMaxIndexerOutboxPayloadBytes);
  ASSERT_TRUE(at_limit.has_value()) << at_limit.error();
  EXPECT_TRUE(validate_indexer_outbox_file_size(*at_limit).has_value());

  auto payload_too_large =
      indexer_outbox_file_size_for_payload(kMaxIndexerOutboxPayloadBytes + 1);
  ASSERT_FALSE(payload_too_large.has_value());
  EXPECT_NE(payload_too_large.error().find("durable limit"), std::string::npos);
  EXPECT_FALSE(validate_indexer_outbox_file_size(kMaxIndexerOutboxFileBytes + 1)
                   .has_value());
}

TEST_F(IndexerOutboxTest, FinalSymlinkIsRejected)
{
  IndexerOutboxStore store = open_store(dir_);
  const std::vector<uint8_t> bytes = {0, 0, 0, 0};
  write_bytes(dir_, "target", bytes);
  const std::string filename = IndexerOutboxStore::candidate_filename(52);
  ASSERT_EQ(symlink("target", path_for(dir_, filename).c_str()), 0);

  auto result = store.load_candidate(52)();
  auto publish_result =
      store.publish_candidate(make_candidate(52, 19, {}, {}))();

  ASSERT_FALSE(result.has_value());
  ASSERT_FALSE(publish_result.has_value());
}

TEST_F(IndexerOutboxTest, RawEnumerationReturnsSortedRecognizedHeights)
{
  IndexerOutboxStore store = open_store(dir_);
  ConsensusValueHash previous{};
  auto first = make_committed(1, {}, {});
  first.consensus_value.prev_hash = previous;
  initialize_test_origin(store, first);
  for (BlockHeight height : {1, 2, 3}) {
    auto committed = make_committed(height, {}, {});
    committed.consensus_value.prev_hash = previous;
    ASSERT_TRUE(store.publish_committed(committed)().has_value());
    previous = sha256(committed.consensus_value);
  }
  ASSERT_TRUE(
      store.publish_candidate(make_candidate(53, 20, {}, {}))().has_value());
  const std::vector<uint8_t> ignored = {0, 0, 0, 0};
  write_bytes(dir_, "committed-00000000000000000004.outbox.tmp.1.2", ignored);
  write_bytes(dir_, "committed-not-a-height.outbox", ignored);
  write_bytes(dir_, "notes.txt", ignored);

  auto heights = store.enumerate_committed_heights_raw();

  ASSERT_TRUE(heights.has_value()) << heights.error();
  EXPECT_EQ(*heights, (std::vector<BlockHeight>{1, 2, 3}));
}

TEST_F(IndexerOutboxTest, EventRootMatchesEventBlockContext)
{
  ProposedTransaction body_a = make_body(8);
  ProposedTransaction body_b = make_body(9);
  TxEventLog event_a = make_event(body_a, 0x77);
  TxEventLog event_b = make_event(body_b, 0x88, /*with_emitted_event=*/false);
  TxEventLogBatch events{event_a, event_b};
  ProposedTransactionBatch bodies{body_a, body_b};

  EventBlockContext context(dir_.directory(), 100);
  context.executor_open_block();
  ASSERT_TRUE(context.add_event(event_a, body_a)());
  ASSERT_TRUE(context.add_event(event_b, body_b)());
  context.executor_close_block();
  context.finish_file_write()();
  const SHA256Hash production_root = context.get_root_hash();
  context.finish_file(production_root)();
  EXPECT_EQ(compute_indexer_outbox_event_root(events, bodies), production_root);

  EventBlockContext empty_context(dir_.directory(), 101);
  empty_context.finish_file_write()();
  const SHA256Hash empty_production_root = empty_context.get_root_hash();
  empty_context.finish_file(empty_production_root)();
  EXPECT_EQ(compute_indexer_outbox_event_root({}, {}), empty_production_root);
}

TEST_F(IndexerOutboxTest,
       EventRootBindsNondeterministicDataButNotSignatureWrapper)
{
  ProposedTransaction body = make_body(10);
  TxEventLog event = make_event(body, 0x99);
  const EventsHash baseline =
      compute_indexer_outbox_event_root({event}, {body});

  ProposedTransaction changed_nondeterministic_data = body;
  changed_nondeterministic_data.nondeterministic_proposal_data
      .nondeterministic_bits[0] ^= 0xff;
  EXPECT_NE(compute_indexer_outbox_event_root({event},
                                              {changed_nondeterministic_data}),
            baseline);

  ProposedTransaction changed_signatures = body;
  changed_signatures.signed_tx.signatures.push_back(DecoratedSignature{});
  EXPECT_EQ(compute_indexer_outbox_event_root({event}, {changed_signatures}),
            baseline);
}

TEST_F(IndexerOutboxTest, EventLeafSerializesEventBeforeNondeterministicData)
{
  ProposedTransaction body = make_body(11);
  TxEventLog event = make_event(body, 0xaa);
  x23::Buf actual;
  const size_t event_xdr_size =
      serialize_event_commitment_leaf(actual, event,
                                      body.nondeterministic_proposal_data);

  x23::Buf expected = x23::serialize(event);
  ASSERT_EQ(event_xdr_size, expected.size());
  x23::serialize_to(&expected, body.nondeterministic_proposal_data);
  EXPECT_EQ(actual, expected);
}

TEST_F(IndexerOutboxTest, CommittedValidationRejectsDuplicateTransactionHash)
{
  ProposedTransaction body = make_body(12);
  auto payload =
      make_committed(20, {body}, {make_event(body, /*marker=*/0xbb)});
  TxEventLog duplicate_event = payload.event_logs.front();
  ProposedTransaction duplicate_body = payload.tx_bodies.front();
  payload.event_logs.push_back(std::move(duplicate_event));
  payload.tx_bodies.push_back(std::move(duplicate_body));

  auto result = validate_indexer_committed_payload(payload)();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("duplicate"), std::string::npos)
      << result.error();
}

} // namespace
} // namespace hogd
