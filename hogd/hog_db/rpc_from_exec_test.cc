// Diagnostics tests for HogDbExecutorRpc::try_push_new_subtransaction (#2759).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "precompiles.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/preserialized_reply.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/hog_db/storage_item_reply.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr std::size_t kTestRamBytes = 64UL * 1024 * 1024;

HogDbDAO
make_dao()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(dir.has_value());
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(),
                     Database::estimate_heap_size(kTestRamBytes), 0);
  CHECK(db.has_value());
  return HogDbDAO(RAMDB(std::move(*db)));
}

AddressAndKey
make_data_key(ContractAddress const &addr, std::uint8_t tag)
{
  StorageKey key(StorageKeyType::ContractData);
  key.contract_data_key()[0] = tag;
  return AddressAndKey{.address = addr, .key = std::move(key)};
}

FlaggedStorageDelta
make_bytes_delta(AddressAndKey addrkey, std::uint8_t byte)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
  delta.bytestring().push_back(byte);
  return FlaggedStorageDelta{.addrkey = std::move(addrkey),
                             .delta = std::move(delta),
                             .flags =
                                 std::to_underlying(StorageDeltaFlags::None)};
}

AddressAndKey
make_balance_key(ContractAddress const &addr)
{
  return AddressAndKey{.address = addr,
                       .key =
                           StorageKey(StorageKeyType::FreeNativeTokenBalance)};
}

FlaggedStorageDelta
make_balance_delta(ContractAddress const &addr, int128_t amount)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
  delta.nnint_add() = to_xdr(amount);
  return FlaggedStorageDelta{.addrkey = make_balance_key(addr),
                             .delta = std::move(delta),
                             .flags =
                                 std::to_underlying(StorageDeltaFlags::None)};
}

// Requests to consume [uniquifier] for [addr]. The request itself succeeds
// (a fresh replay-cache entry); it's the balance lock it charges storage
// cost against that can later fail at sync_balance_locks.
FlaggedStorageDelta
make_consume_uniquifier_delta(ContractAddress const &addr,
                              Uniquifier uniquifier)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeConsumeUniquifier);
  delta.consume_uniquifier_args().uniquifier = uniquifier;
  return FlaggedStorageDelta{
      .addrkey = AddressAndKey{.address = addr,
                               .key = StorageKey(StorageKeyType::ReplayCache)},
      .delta = std::move(delta),
      .flags = std::to_underlying(StorageDeltaFlags::None)};
}

FlaggedStorageDelta
make_deploy_delta(ContractAddress const &addr, std::uint8_t hash_byte,
                  StorageDeltaFlagSet flags)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
  DeployedContractHash hash{};
  hash.hash.fill(hash_byte);
  hash.contract_size = 32;
  hash.contract_type = ContractType::kRv32;
  delta.deployed_contract_hash() = hash;
  return FlaggedStorageDelta{
      .addrkey = AddressAndKey{.address = addr,
                               .key = StorageKey(StorageKeyType::Metadata)},
      .delta = std::move(delta),
      .flags = flags};
}

class RpcFromExecDiagnosticsTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    HogDbDAO dao = make_dao();
    for (std::size_t i = 0; i < addr_.size(); ++i)
      addr_[i] = static_cast<std::uint8_t>(i + 1);
    ASSERT_TRUE(dao.write_address_balance(addr_, int128_t{1'000'000'000}));
    ctx_ = std::make_unique<RPCContext>(std::move(dao), nullptr);
    ctx_->initialize_cost_model(StorageCostModel::default_cost_model());
    ctx_->set_network_config(stock_network_config());
    ASSERT_TRUE(
        ctx_->open_block_tx(BlockID{1},
                            BlockExecutionMode::kBlockExecutionModeProposal));
    rpc_ = std::make_unique<HogDbExecutorRpc>(*ctx_);
  }

  // Pushes a winning write at [key] from [tx] in [block] so a differing
  // write at the same key conflicts.
  void seed_conflict(BlockID block, TransactionID tx, AddressAndKey const &key)
  {
    StorageDeltaBatch batch;
    batch.push_back(make_bytes_delta(key, 0xAA));
    auto r = rpc_->try_push_new_subtransaction(block, tx, batch, {});
    ASSERT_EQ(r.error(), RPCError::kRPCNone);
  }

  ContractAddress addr_{};
  std::unique_ptr<RPCContext> ctx_;
  std::unique_ptr<HogDbExecutorRpc> rpc_;
};

TEST_F(RpcFromExecDiagnosticsTest, ValidationModeReturnsNoDiagnostics)
{
  ASSERT_TRUE(
      ctx_->open_block_tx(BlockID{2},
                          BlockExecutionMode::kBlockExecutionModeValidation));

  auto key = make_data_key(addr_, 1);
  seed_conflict(BlockID{2}, TransactionID{1}, key);

  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key, 0xBB));
  auto r = rpc_->try_push_new_subtransaction(BlockID{2}, TransactionID{2},
                                             batch, {});
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);
  EXPECT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticNone);
}

TEST_F(RpcFromExecDiagnosticsTest, ProposalModeReportsConflicts)
{
  auto key_a = make_data_key(addr_, 1);
  auto key_b = make_data_key(addr_, 2);
  seed_conflict(BlockID{1}, TransactionID{1}, key_a);
  {
    // Second winner, separate tx, second key.
    StorageDeltaBatch batch;
    batch.push_back(make_bytes_delta(key_b, 0xAA));
    auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2},
                                               batch, {});
    ASSERT_EQ(r.error(), RPCError::kRPCNone);
  }

  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key_a, 0xBB));
  batch.push_back(make_bytes_delta(key_b, 0xBB));
  auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{3},
                                             batch, {});
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);
  if constexpr (is_debug_build()) {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticAll);
    ASSERT_EQ(r.conflict().conflicts().size(), 2U);
    // key_a then key_b, both ValueMismatch (order: hog_db preserves batch
    // order).
    EXPECT_EQ(r.conflict().conflicts()[0].addrkey, key_a);
    EXPECT_EQ(r.conflict().conflicts()[1].addrkey, key_b);
    for (auto const &d : r.conflict().conflicts()) {
      EXPECT_EQ(d.code, DbConflictCode::ValueMismatch);
    }
  }
  else {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticFirst);
    EXPECT_EQ(r.conflict().first().addrkey, key_a);
    EXPECT_EQ(r.conflict().first().code, DbConflictCode::ValueMismatch);
  }
}

TEST_F(RpcFromExecDiagnosticsTest, ProposalModeReportsAllBalanceConflicts)
{
  // Neither address below has a seeded balance (unlike addr_ from SetUp), so
  // consuming a uniquifier locks a positive storage cost against a balance
  // delta that starts at zero, driving it negative and failing bounds at
  // sync -- for both addresses, since the per-delta requests themselves
  // succeed and only balance sync fails.
  ContractAddress addr_a{};
  ContractAddress addr_b{};
  for (std::size_t i = 0; i < addr_a.size(); ++i) {
    addr_a[i] = static_cast<std::uint8_t>(0x10 + i);
    addr_b[i] = static_cast<std::uint8_t>(0x30 + i);
  }

  StorageDeltaBatch batch;
  batch.push_back(make_consume_uniquifier_delta(
      addr_a, Uniquifier{.block_expiry_height = BlockHeight{100}, .nonce = 1}));
  batch.push_back(make_consume_uniquifier_delta(
      addr_b, Uniquifier{.block_expiry_height = BlockHeight{100}, .nonce = 2}));

  auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{5},
                                             batch, {});
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);

  if constexpr (is_debug_build()) {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticAll);
    ASSERT_EQ(r.conflict().conflicts().size(), 2U);
    std::set<ContractAddress> expected{addr_a, addr_b};
    std::set<ContractAddress> actual;
    for (auto const &d : r.conflict().conflicts()) {
      EXPECT_EQ(d.code, DbConflictCode::InsufficientFunds);
      EXPECT_EQ(d.addrkey.key.key_type(),
                StorageKeyType::FreeNativeTokenBalance);
      actual.insert(d.addrkey.address);
    }
    EXPECT_EQ(expected, actual);
  }
  else {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticFirst);
    EXPECT_EQ(r.conflict().first().code, DbConflictCode::InsufficientFunds);
    EXPECT_EQ(r.conflict().first().addrkey.key.key_type(),
              StorageKeyType::FreeNativeTokenBalance);
    EXPECT_TRUE(r.conflict().first().addrkey.address == addr_a ||
                r.conflict().first().addrkey.address == addr_b);
  }
}

TEST_F(RpcFromExecDiagnosticsTest, FailedPushRollsBackRegardlessOfLevel)
{
  auto key_a = make_data_key(addr_, 1);
  auto key_b = make_data_key(addr_, 2);
  seed_conflict(BlockID{1}, TransactionID{1}, key_a);

  // Tx 3: one good delta (key_b) + one conflicting delta (key_a).
  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key_b, 0xCC));
  batch.push_back(make_bytes_delta(key_a, 0xBB));
  auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{3},
                                             batch, {});
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);

  // The whole subtransaction rolled back: tx 4 can now claim key_b with
  // different bytes (no residue from tx 3's successful key_b delta).
  StorageDeltaBatch retry;
  retry.push_back(make_bytes_delta(key_b, 0xDD));
  auto r2 = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{4},
                                              retry, {});
  EXPECT_EQ(r2.error(), RPCError::kRPCNone);
}

// A failed push must leave the root InvokeTX holding nothing for the keys
// only the failed child touched. Retained empty entries are invisible to
// commit/abort/sync, but rollback skips empty vectors, so they would survive
// every later rollback -- letting a contract that repeats a failing TryInvoke
// against fresh addresses grow the live transaction's maps (#3090).
TEST_F(RpcFromExecDiagnosticsTest, FailedBalancePushRetainsNoEntries)
{
  // Under the cost model's balance cost, so crediting a fresh receiver never
  // pays for its own balance row: its lock is rejected on first admission in
  // update_balance_lock(), before any entry can be published.
  constexpr int128_t kTransferAmount = 100;
  ASSERT_LT(kTransferAmount,
            StorageCostModel::default_cost_model().get_balance_cost());

  constexpr int kAttempts = 8;
  for (int i = 0; i < kAttempts; ++i) {
    ContractAddress receiver{};
    for (std::size_t j = 0; j < receiver.size(); ++j)
      receiver[j] = static_cast<std::uint8_t>(0xB0 + j);
    receiver[0] = static_cast<std::uint8_t>(i);

    // addr_ is funded, so the debit locks successfully and is then rolled
    // back; the fresh receiver's credit is what fails.
    StorageDeltaBatch batch;
    batch.push_back(make_balance_delta(addr_, -kTransferAmount));
    batch.push_back(make_balance_delta(receiver, kTransferAmount));

    auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{9},
                                               batch, {});
    ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed) << i;

    InvokeTX *itx = ctx_->get_invoke_tx(BlockID{1}, TransactionID{9});
    ASSERT_NE(itx, nullptr);
    EXPECT_EQ(itx->get_savepoint_head(), 0U) << i;
    EXPECT_EQ(itx->change_entry_count_for_testing(), 0U) << i;
    EXPECT_EQ(itx->balance_entry_count_for_testing(), 0U) << i;
  }
}

// Same invariant for ordinary storage keys: the successful key_b write and
// the storage cost it locked against addr_'s balance are both rolled back,
// and neither leaves an entry behind.
TEST_F(RpcFromExecDiagnosticsTest, FailedDataPushRetainsNoEntries)
{
  auto key_a = make_data_key(addr_, 1);
  auto key_b = make_data_key(addr_, 2);
  seed_conflict(BlockID{1}, TransactionID{1}, key_a);

  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key_b, 0xCC));
  batch.push_back(make_bytes_delta(key_a, 0xBB));
  auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{3},
                                             batch, {});
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);

  InvokeTX *itx = ctx_->get_invoke_tx(BlockID{1}, TransactionID{3});
  ASSERT_NE(itx, nullptr);
  EXPECT_EQ(itx->get_savepoint_head(), 0U);
  EXPECT_EQ(itx->change_entry_count_for_testing(), 0U);
  EXPECT_EQ(itx->balance_entry_count_for_testing(), 0U);
}

// Regression test for #1189: request_one dropped [flags] on the floor for
// kDeltaTypeDeployContract deltas, so an exclusive lock requested on a deploy
// was silently downgraded to an ordinary reservation -- MockDB blocked the
// second deploy, hog_db did not. The two tests below pin both halves: the
// flag now blocks, and its absence still admits an identical concurrent
// deploy (so the block above comes from the flag, not from the deploy path
// having become conflict-happy).
TEST_F(RpcFromExecDiagnosticsTest, DeployContractExclusiveFlagBlocksConcurrent)
{
  StorageDeltaBatch exclusive;
  exclusive.push_back(
      make_deploy_delta(addr_, 0xD1,
                        std::to_underlying(StorageDeltaFlags::ExclusiveLock)));
  auto r1 = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{1},
                                              exclusive, {});
  ASSERT_EQ(r1.error(), RPCError::kRPCNone);

  // Same address, same contract hash: without the exclusive lock this would
  // be admitted alongside (see the control test below).
  StorageDeltaBatch concurrent;
  concurrent.push_back(
      make_deploy_delta(addr_, 0xD1,
                        std::to_underlying(StorageDeltaFlags::None)));
  auto r2 = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2},
                                              concurrent, {});
  ASSERT_EQ(r2.error(), RPCError::kRPCStorageWriteFailed);

  if constexpr (is_debug_build()) {
    ASSERT_EQ(r2.conflict().level(), DbDiagnosticLevel::kDbDiagnosticAll);
    ASSERT_EQ(r2.conflict().conflicts().size(), 1U);
    EXPECT_EQ(r2.conflict().conflicts()[0].addrkey.key.key_type(),
              StorageKeyType::Metadata);
    EXPECT_EQ(r2.conflict().conflicts()[0].code,
              DbConflictCode::ExclusiveLockDenied);
  }
  else {
    ASSERT_EQ(r2.conflict().level(), DbDiagnosticLevel::kDbDiagnosticFirst);
    EXPECT_EQ(r2.conflict().first().addrkey.key.key_type(),
              StorageKeyType::Metadata);
    EXPECT_EQ(r2.conflict().first().code, DbConflictCode::ExclusiveLockDenied);
  }
}

TEST_F(RpcFromExecDiagnosticsTest, DeployContractWithoutFlagAdmitsConcurrent)
{
  StorageDeltaBatch first;
  first.push_back(
      make_deploy_delta(addr_, 0xD2,
                        std::to_underlying(StorageDeltaFlags::None)));
  auto r1 = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{1},
                                              first, {});
  ASSERT_EQ(r1.error(), RPCError::kRPCNone);

  StorageDeltaBatch second;
  second.push_back(
      make_deploy_delta(addr_, 0xD2,
                        std::to_underlying(StorageDeltaFlags::None)));
  auto r2 = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2},
                                              second, {});
  EXPECT_EQ(r2.error(), RPCError::kRPCNone);
}

// A persistent contract-code row that is not even a whole number of XDR
// words (e.g. tampered on-disk state) must surface as a structured corruption
// error rather than go out in a reply x23 cannot size. See #3804.
TEST_F(RpcFromExecDiagnosticsTest, MalformedContractRowReportsCorruption)
{
  SHA256Hash hash{};
  hash.fill(0x98);
  constexpr std::array<std::byte, 3> kTruncatedContractXdr{std::byte{0},
                                                           std::byte{0},
                                                           std::byte{0}};
  ASSERT_TRUE(ctx_->get_db().write_contract_code(hash, kTruncatedContractXdr));

  auto result = rpc_->get_contract(BlockID{1}, hash);
  EXPECT_EQ(result.error(), RPCError::kRPCUnrecoverable);
}

// BorrowedQueueEntryReply must put the same bytes on the wire as the
// ExpectedQueueEntry it stands in for, on every arm and length (#2035).
TEST(BorrowedQueueEntryReplyTest, MatchesExpectedQueueEntryOnTheWire)
{
  static_assert(
      x23::serializes_like<BorrowedQueueEntryReply, ExpectedQueueEntry>);
  static_assert(x23::satisfies_rpc_return<BorrowedQueueEntryReply,
                                          ExpectedQueueEntry, x23::MaybeAsync>);

  // 0 and kQueueEntryMaxLen are the bounds; 1 and 3 are the sub-word cases
  // and 13 is the interior case that needs padding.
  for (size_t len : {size_t{0}, size_t{1}, size_t{3}, size_t{13},
                     size_t{kQueueEntryMaxLen}}) {
    QueueEntry entry;
    for (size_t i = 0; i < len; i++) {
      entry.push_back(static_cast<uint8_t>((i * 7) + 1));
    }

    ExpectedQueueEntry owning;
    owning.error(RPCError::kRPCNone);
    owning.queue_entry() = entry;

    BorrowedQueueEntryReply borrowed(RPCError::kRPCNotFound);
    borrowed.set_entry(x23::BufView(entry.data(), entry.size()));

    EXPECT_EQ(borrowed.error(), RPCError::kRPCNone);
    EXPECT_EQ(x23::xdr_size(owning), xdr_size(borrowed)) << "length " << len;
    auto owning_bytes = x23::serialize(owning);
    auto borrowed_bytes = x23::serialize(borrowed);
    ASSERT_EQ(owning_bytes.size(), borrowed_bytes.size()) << "length " << len;
    EXPECT_EQ(0, memcmp(owning_bytes.data(), borrowed_bytes.data(),
                        owning_bytes.size()))
        << "length " << len;
  }

  // Every other discriminant selects the void arm, which carries no entry.
  for (RPCError error : {RPCError::kRPCNotFound, RPCError::kRPCBadBlockID,
                         RPCError::kRPCUnrecoverable}) {
    ExpectedQueueEntry owning;
    owning.error(error);
    BorrowedQueueEntryReply borrowed(error);

    EXPECT_EQ(x23::xdr_size(owning), xdr_size(borrowed));
    auto owning_bytes = x23::serialize(owning);
    auto borrowed_bytes = x23::serialize(borrowed);
    ASSERT_EQ(owning_bytes.size(), borrowed_bytes.size());
    EXPECT_EQ(0, memcmp(owning_bytes.data(), borrowed_bytes.data(),
                        owning_bytes.size()));
  }
}

// Bytes 0..len-1 in a pattern that makes a misplaced or dropped byte show.
std::vector<std::byte>
patterned_bytes(size_t len)
{
  std::vector<std::byte> bytes(len);
  for (size_t i = 0; i < len; i++) {
    bytes[i] = static_cast<std::byte>((i * 7) + 1);
  }
  return bytes;
}

// An OwningSpan holding a copy of [bytes].
OwningSpan<const std::byte>
owning_copy(std::span<const std::byte> bytes)
{
  std::vector<std::byte> buf(bytes.begin(), bytes.end());
  auto span = std::span<const std::byte>(buf);
  return OwningSpan(span, [ref = std::move(buf)] {});
}

// Decodes [reply] as the executor does, from the bytes x23 puts on the wire,
// after checking those bytes are exactly what the [Wire] they decode to
// encodes as.
template<typename Wire>
Wire
decode_reply(const auto &reply)
{
  static_assert(x23::serializes_like<decltype(reply), Wire>);
  x23::Buf wire = x23::serialize(reply);
  EXPECT_EQ(wire.size(), xdr_size(reply));
  Wire decoded;
  x23::deserialize(&decoded, x23::BufViewEnd(wire));
  x23::Buf reencoded = x23::serialize(decoded);
  EXPECT_TRUE(std::ranges::equal(wire, reencoded));
  return decoded;
}

// The padding cases (1 and 3 bytes short of a word, and 13 in the interior)
// and both bounds of a Bytestring.
constexpr std::array<size_t, 5> kBytestringLengths{0, 1, 3, 13,
                                                   static_cast<size_t>(
                                                       kBytestringCeilingLen)};

TEST(StorageItemReplyTest, MatchesExpectedStorageItemOnTheWire)
{
  using Reply = StorageItemReply<ExpectedStorageItem>;
  static_assert(
      x23::satisfies_rpc_return<Reply, ExpectedStorageItem, x23::MaybeAsync>);

  for (size_t len : kBytestringLengths) {
    std::vector<std::byte> bytes = patterned_bytes(len);
    ExpectedStorageItem owning;
    owning.error(RPCError::kRPCNone);
    owning.item().type(StorageItemType::kItemTypeBytestring);
    for (std::byte b : bytes) {
      owning.item().bytestring().push_back(std::to_integer<uint8_t>(b));
    }

    Reply borrowed = Reply::bytestring(owning_copy(bytes));
    EXPECT_EQ(borrowed.error(), RPCError::kRPCNone);
    EXPECT_EQ(x23::xdr_size(owning), xdr_size(borrowed)) << "length " << len;
    EXPECT_TRUE(
        std::ranges::equal(x23::serialize(owning), x23::serialize(borrowed)))
        << "length " << len;
  }

  // Anything else travels as the ExpectedStorageItem it is.
  WireStorageItem nnint(StorageItemType::kItemTypeNonnegativeInt);
  nnint.nnint() = to_xdr(int128_t{42});
  ExpectedStorageItem owning(RPCError::kRPCNone);
  owning.item() = nnint;
  Reply reply{ExpectedStorageItem(owning)};
  EXPECT_TRUE(
      std::ranges::equal(x23::serialize(owning), x23::serialize(reply)));

  for (RPCError error : {RPCError::kRPCNotFound, RPCError::kRPCBadBlockID,
                         RPCError::kRPCUnrecoverable}) {
    Reply failed(error);
    EXPECT_EQ(failed.error(), error);
    EXPECT_TRUE(std::ranges::equal(x23::serialize(ExpectedStorageItem(error)),
                                   x23::serialize(failed)));
  }
}

TEST_F(RpcFromExecDiagnosticsTest, SnapshotBytestringReachesClientIntact)
{
  for (size_t i = 0; i < kBytestringLengths.size(); i++) {
    size_t len = kBytestringLengths[i];
    AddressAndKey key = make_data_key(addr_, static_cast<uint8_t>(0x60 + i));
    std::vector<std::byte> bytes = patterned_bytes(len);
    ASSERT_TRUE(ctx_->get_db().write_bytes(make_item_key(key), bytes));

    auto reply = rpc_->get_snapshot_item(BlockID{1}, key);
    ASSERT_NE(reply.bytestring(), nullptr) << "length " << len;
    auto item = decode_reply<ExpectedStorageItem>(reply);
    ASSERT_EQ(item.error(), RPCError::kRPCNone);
    ASSERT_EQ(item.item().type(), StorageItemType::kItemTypeBytestring);
    EXPECT_TRUE(
        std::ranges::equal(std::as_bytes(std::span(item.item().bytestring())),
                           bytes))
        << "length " << len;
  }

  auto missing =
      rpc_->get_snapshot_item(BlockID{1}, make_data_key(addr_, 0x5F));
  EXPECT_EQ(decode_reply<ExpectedStorageItem>(missing).error(),
            RPCError::kRPCNotFound);

  auto balance = rpc_->get_snapshot_item(BlockID{1}, make_balance_key(addr_));
  auto balance_item = decode_reply<ExpectedStorageItem>(balance);
  ASSERT_EQ(balance_item.error(), RPCError::kRPCNone);
  EXPECT_EQ(balance_item.item().type(),
            StorageItemType::kItemTypeNonnegativeInt);
}

// A stored row longer than any Bytestring is corrupt state: the reply carries
// the error, not an opaque the executor's decode would reject.
TEST_F(RpcFromExecDiagnosticsTest, OversizedBytestringRowReportsCorruption)
{
  AddressAndKey key = make_data_key(addr_, 0x70);
  std::vector<std::byte> bytes =
      patterned_bytes(static_cast<size_t>(kBytestringCeilingLen) + 4);
  ASSERT_TRUE(ctx_->get_db().write_bytes(make_item_key(key), bytes));

  auto reply = rpc_->get_snapshot_item(BlockID{1}, key);
  EXPECT_EQ(reply.bytestring(), nullptr);
  EXPECT_EQ(decode_reply<ExpectedStorageItem>(reply).error(),
            RPCError::kRPCUnrecoverable);
}

Contract
make_rv32_contract(size_t len)
{
  Contract contract(ContractType::kRv32);
  for (std::byte b : patterned_bytes(len)) {
    contract.rv32_contract().data.push_back(std::to_integer<uint8_t>(b));
  }
  return contract;
}

// get_contract serves the stored row as is, so whatever is stored must reach
// the executor as the Contract it encodes: inline and chunked rows, every
// padding case, and each arm.
TEST_F(RpcFromExecDiagnosticsTest, ContractRowReachesClientIntact)
{
  std::vector<Contract> contracts;
  for (size_t len :
       {size_t{0}, size_t{1}, size_t{3}, size_t{13}, size_t{200'000}}) {
    contracts.push_back(make_rv32_contract(len));
  }
  contracts.emplace_back(ContractType::kMockTest);
  contracts.back().mock_contract_id() = 7;
  contracts.emplace_back(ContractType::kNative);
  contracts.back().native_contract_id() =
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet;

  for (size_t i = 0; i < contracts.size(); i++) {
    SHA256Hash hash{};
    hash.fill(static_cast<uint8_t>(0x10 + i));
    x23::Buf row = x23::serialize(contracts[i]);
    ASSERT_TRUE(ctx_->get_db().write_contract_code(hash, std::as_bytes(
                                                             std::span(row))));

    auto reply = rpc_->get_contract(BlockID{1}, hash);
    ASSERT_EQ(reply.error(), RPCError::kRPCNone) << "contract " << i;
    auto result = decode_reply<ExpectedContract>(reply);
    ASSERT_EQ(result.error(), RPCError::kRPCNone);
    EXPECT_TRUE(std::ranges::equal(x23::serialize(result.contract()), row))
        << "contract " << i;
  }

  SHA256Hash missing{};
  missing.fill(0x9F);
  auto reply = rpc_->get_contract(BlockID{1}, missing);
  EXPECT_EQ(decode_reply<ExpectedContract>(reply).error(),
            RPCError::kRPCNotFound);
}

// A push folds the runs it is given once it succeeds, so a single pop then
// reverts every subtransaction in the run.
TEST_F(RpcFromExecDiagnosticsTest, FoldedPushIsPoppedAsOne)
{
  auto key = make_data_key(addr_, 1);
  for (std::uint8_t i = 1; i <= 2; ++i) {
    StorageDeltaBatch batch;
    batch.push_back(make_bytes_delta(key, i));
    auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{1},
                                               batch, {});
    ASSERT_EQ(r.error(), RPCError::kRPCNone);
  }
  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key, 3));
  SubtransactionFoldRuns fold;
  fold.push_back(3);
  auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{1},
                                             batch, fold);
  ASSERT_EQ(r.error(), RPCError::kRPCNone);

  InvokeTX *itx = ctx_->get_invoke_tx(BlockID{1}, TransactionID{1});
  ASSERT_NE(itx, nullptr);
  EXPECT_EQ(itx->get_savepoint_head(), 1U);
  auto stored = itx->read_bytes(make_item_key(key));
  ASSERT_TRUE(stored);
  EXPECT_TRUE(std::ranges::equal(*stored, std::array{std::byte{3}}));

  ASSERT_EQ(rpc_->pop_subtransactions(BlockID{1}, TransactionID{1}, 1),
            RPCError::kRPCNone);
  EXPECT_EQ(itx->get_savepoint_head(), 0U);
  EXPECT_EQ(itx->change_entry_count_for_testing(), 0U);
}

// A fold that reaches below the subtransactions the executor pushed is
// executor misbehavior, refused before anything is pushed.
TEST_F(RpcFromExecDiagnosticsTest, FoldPastStackIsRejected)
{
  auto key = make_data_key(addr_, 1);
  seed_conflict(BlockID{1}, TransactionID{1}, key);

  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(make_data_key(addr_, 2), 1));
  SubtransactionFoldRuns fold;
  fold.push_back(2);
  fold.push_back(1);
  auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{1},
                                             batch, fold);
  EXPECT_EQ(r.error(), RPCError::kRPCUnrecoverable);

  InvokeTX *itx = ctx_->get_invoke_tx(BlockID{1}, TransactionID{1});
  ASSERT_NE(itx, nullptr);
  EXPECT_EQ(itx->get_savepoint_head(), 1U);
  EXPECT_TRUE(itx->read_bytes(make_item_key(key)));
  EXPECT_EQ(
      itx->read_bytes(make_item_key(make_data_key(addr_, 2))).error().code(),
      HogDbCode::kNotFound);
}

// A push that fails folds nothing: the subtransactions below it stay for the
// executor to pop one frame at a time.
TEST_F(RpcFromExecDiagnosticsTest, FailedPushDoesNotFold)
{
  auto contested = make_data_key(addr_, 1);
  seed_conflict(BlockID{1}, TransactionID{1}, contested);

  for (std::uint8_t i = 2; i <= 3; ++i) {
    StorageDeltaBatch batch;
    batch.push_back(make_bytes_delta(make_data_key(addr_, i), i));
    auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2},
                                               batch, {});
    ASSERT_EQ(r.error(), RPCError::kRPCNone);
  }
  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(contested, 0xBB));
  SubtransactionFoldRuns fold;
  fold.push_back(3);
  auto r = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2},
                                             batch, fold);
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);

  InvokeTX *itx = ctx_->get_invoke_tx(BlockID{1}, TransactionID{2});
  ASSERT_NE(itx, nullptr);
  EXPECT_EQ(itx->get_savepoint_head(), 2U);
  ASSERT_EQ(rpc_->pop_subtransactions(BlockID{1}, TransactionID{2}, 1),
            RPCError::kRPCNone);
  EXPECT_TRUE(itx->read_bytes(make_item_key(make_data_key(addr_, 2))));
  EXPECT_EQ(
      itx->read_bytes(make_item_key(make_data_key(addr_, 3))).error().code(),
      HogDbCode::kNotFound);
}

} // namespace

} // namespace hogd
