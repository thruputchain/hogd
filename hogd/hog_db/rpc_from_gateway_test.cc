// Smoke tests for HogDbGatewayRpc.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

// xdr types
#include "consensus.hh"
#include "db_gateway.hh"
#include "network_config.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hog_exec_constants.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "hogd/hog_db/background_commit_worker.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_gateway.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/rpc.h"
#include "x23/rpc_msg.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

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

// Builds the address+key pair that locates a HOG balance.  The
// FreeNativeTokenBalance key has no extra payload — the address alone
// disambiguates.
AddressAndKey
make_balance_query(ContractAddress const &addr)
{
  AddressAndKey query;
  query.address = addr;
  query.key.key_type(StorageKeyType::FreeNativeTokenBalance);
  return query;
}

// Decodes [reply] as a client does, from the bytes x23 puts on the wire.
DbGatewayResult
decode(const DbGatewayItemsReply &reply)
{
  x23::Buf wire = x23::serialize(reply);
  DbGatewayResult result;
  x23::deserialize(&result, x23::BufViewEnd(wire));
  return result;
}

} // namespace

TEST(HogDbGatewayRpcTest, GetItemsReturnsSeededBalance)
{
  HogDbDAO dao = make_dao();

  // Non-zero address so it can't collide with the zero address used by
  // the missing-balance test below.
  ContractAddress addr{};
  for (std::size_t i = 0; i < addr.size(); ++i)
    addr[i] = static_cast<std::uint8_t>(i + 1);
  ASSERT_TRUE(dao.write_address_balance(addr, int128_t{12345}).has_value());

  RPCContext ctx(std::move(dao), nullptr);

  AddressAndKeyBatch batch;
  batch.push_back(make_balance_query(addr));

  HogDbGatewayRpc gateway(ctx);
  DbGatewayResult result = decode(gateway.dbgateway_get_items(batch)());

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items[0].item().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(result.payload().items[0].item().nnint()),
            int128_t{12345});
}

TEST(HogDbGatewayRpcTest, ConfigPreservesStatedValuesAndResolvesDefaults)
{
  HogDbDAO dao = make_dao();
  auto costs = StorageCostModel::default_cost_model().to_xdr();
  costs.balance_cost = std::numeric_limits<uint64_t>::max();
  costs.contract_deployment_cost_per_byte = 0;
  ASSERT_TRUE(dao.write_cost_model(StorageCostModel(costs)));
  RPCContext ctx(std::move(dao), nullptr);
  DbTip tip{};
  tip.value.proposal_header.block_height = BlockHeight{42};
  tip.network_config.v(0);
  NetworkConfigParams::set_param(tip.network_config,
                                 ConfigParamID::kConfigParamMaxSHA256Buffers,
                                 7);
  ctx.set_tip(tip);
  HogDbGatewayRpc gateway(ctx);
  auto result = gateway.dbgateway_get_config()();
  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  EXPECT_EQ(result.config().tip, tip);
  EXPECT_EQ(result.config().storage_cost_model, costs);
  EXPECT_EQ(result.config().tip.network_config.v0().params.size(), 1U);
  EXPECT_EQ(NetworkConfigParams(result.config().effective_config.v0()),
            NetworkConfigParams(tip.network_config.v0()));
  EXPECT_EQ(result.config().effective_config.v0().params.size(),
            static_cast<size_t>(std::to_underlying(kHighestConfigParamID)) + 1);

  tip.value.proposal_header.block_height = BlockHeight{43};
  NetworkConfigParams::set_param(tip.network_config,
                                 ConfigParamID::kConfigParamMaxSHA256Buffers,
                                 9);
  ctx.set_tip(tip);
  result = gateway.dbgateway_get_config()();
  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  EXPECT_EQ(result.config().tip, tip);
  EXPECT_EQ(NetworkConfigParams(result.config().effective_config.v0())
                .max_sha256_buffers,
            9U);
  EXPECT_EQ(result.config().storage_cost_model, costs);
}

TEST(HogDbGatewayRpcTest, ConfigPricesRemainFixedAfterGenesis)
{
  HogDbDAO dao = make_dao();
  const auto costs = StorageCostModel::default_cost_model().to_xdr();
  ASSERT_TRUE(dao.write_cost_model(StorageCostModel(costs)));
  RPCContext ctx(std::move(dao), nullptr);
  DbTip tip{};
  tip.value.proposal_header.block_height = BlockHeight{42};
  tip.network_config.v(0);
  ctx.set_tip(tip);
  HogDbGatewayRpc gateway(ctx);

  auto replacement = costs;
  replacement.balance_cost = std::numeric_limits<uint64_t>::max();
  ASSERT_FALSE(ctx.get_db().write_cost_model(StorageCostModel(replacement)));
  const auto result = gateway.dbgateway_get_config()();
  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  EXPECT_EQ(result.config().tip, tip);
  EXPECT_EQ(result.config().storage_cost_model, costs);
}

TEST(HogDbGatewayRpcTest, ConfigDoesNotInventMissingPrices)
{
  RPCContext ctx(make_dao(), nullptr);
  HogDbGatewayRpc gateway(ctx);
  DbTip tip{};
  tip.value.proposal_header.block_height = BlockHeight{-1};
  ctx.set_tip(tip);
  EXPECT_EQ(gateway.dbgateway_get_config()().error(), RPCError::kRPCNotFound);
  tip.value.proposal_header.block_height = BlockHeight{0};
  ctx.set_tip(tip);
  EXPECT_EQ(gateway.dbgateway_get_config()().error(),
            RPCError::kRPCUnrecoverable);
}

TEST(HogDbGatewayRpcTest, ConfigWaitsForPublishedTipAndPrices)
{
  RPCContext ctx(make_dao(), nullptr);
  HogDbGatewayRpc gateway(ctx);
  auto publication_guard =
      std::make_unique<RPCContext::CommitPublicationGuard>(ctx, BlockTXID{0});

  auto pending = gateway.dbgateway_get_config();
  EXPECT_FALSE(pending.operator co_await().await_ready());

  // Publish genesis while the read is suspended. Neither the pre-genesis tip
  // nor the missing cost model may be returned before publication finishes.
  const auto costs = StorageCostModel::default_cost_model();
  EXPECT_TRUE(ctx.get_db().write_cost_model(costs));
  DbTip tip{};
  tip.id = BlockTXID{0};
  tip.value.proposal_header.block_height = BlockHeight{0};
  tip.value.proposal_header.config_hash[0] = 42;
  tip.network_config.v(0);
  ctx.set_tip(tip);
  EXPECT_FALSE(pending.operator co_await().await_ready());
  publication_guard.reset();

  const auto result = pending();
  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  EXPECT_EQ(result.config().tip, tip);
  EXPECT_EQ(result.config().storage_cost_model, costs.to_xdr());
  EXPECT_TRUE(result.config().tip.network_config.v0().params.empty());
  EXPECT_EQ(result.config().effective_config,
            NetworkConfigParams::stated_defaults());
}

// Querying an address that was never written returns per-item
// kRPCNotFound while the outer batch result stays kRPCNone.
TEST(HogDbGatewayRpcTest, GetItemsMissingBalanceIsNotFound)
{
  HogDbDAO dao = make_dao();
  RPCContext ctx(std::move(dao), nullptr);

  // Zero address: make_dao() seeds nothing, so this address has no
  // balance row.
  ContractAddress addr{};
  AddressAndKeyBatch batch;
  batch.push_back(make_balance_query(addr));

  HogDbGatewayRpc gateway(ctx);
  DbGatewayResult result = decode(gateway.dbgateway_get_items(batch)());

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  EXPECT_EQ(result.payload().items[0].error(), RPCError::kRPCNotFound);
}

// Regression test for #1715: contract-data NNInt values must be decoded
// from canonical XDR bytes, not reinterpreted as host-endian int128_t.
TEST(HogDbGatewayRpcTest, GetItemsContractDataNNIntDecodesCanonicalBytes)
{
  HogDbDAO dao = make_dao();

  ContractAddress addr{};
  addr[0] = 42;
  ASSERT_TRUE(dao.write_address_balance(addr, int128_t{1'000'000}));

  StorageKey storage_key(StorageKeyType::ContractData);
  storage_key.contract_data_key()[0] = 7;
  AddressAndKey query{.address = addr, .key = std::move(storage_key)};
  ItemKey item_key = make_item_key(query);

  AuthChangeSubscriptionManager subscription_manager;
  BackgroundCommitWorker background_commit_worker(subscription_manager,
                                                  nullptr);
  StorageCostModel cost_model = StorageCostModel::default_cost_model();
  {
    StorageLongHash base_hash = dao.compute_full_db_hash();
    PerThreadARC<BlockTX> block_tx(
        dao, cost_model, background_commit_worker, BlockHeight{1},
        BlockExecutionMode::kBlockExecutionModeProposal,
        static_cast<uint32_t>(kMaxSavepointDepth));
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_nnint128(item_key, int128_t{1}, int128_t{0},
                                           std::to_underlying(
                                               StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());

    auto db_hash = block_tx.get().close(base_hash);
    ASSERT_TRUE(db_hash.has_value());
    ASSERT_TRUE(block_tx.get().commit());
  }

  HogDbStatus<int128_t> dao_value = dao.read_nnint128(item_key);
  ASSERT_TRUE(dao_value);
  ASSERT_EQ(*dao_value, int128_t{1});

  RPCContext ctx(std::move(dao), nullptr);
  AddressAndKeyBatch batch;
  batch.push_back(query);

  HogDbGatewayRpc gateway(ctx);
  DbGatewayResult result = decode(gateway.dbgateway_get_items(batch)());

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items[0].item().type(),
            StorageItemType::kItemTypeNonnegativeInt);

  int128_t gateway_value = from_xdr(result.payload().items[0].item().nnint());
  EXPECT_EQ(gateway_value, int128_t{1});
}

// Exercises the wait-out-publication path. While a commit is mid-publication
// (commit_publishing_ held), a committed-tip read suspends in
// co_wait_for_no_commit_publishing() instead of reading a tip that is
// inconsistent with the just-applied ramdb data; once the publication
// finishes the read proceeds and returns kRPCNone.
//
// Note: releasing the guard here does NOT advance the tip, so after the wait
// the post-read staleness check passes on the first attempt and the retry
// (`continue`) branch is not taken in this test. The staleness decision that
// drives that branch is covered directly by
// CommittedReadStaleDetectsTipAdvance.
TEST(HogDbGatewayRpcTest, GetItemsWaitsOutCommitPublication)
{
  HogDbDAO dao = make_dao();

  ContractAddress addr{};
  for (std::size_t i = 0; i < addr.size(); ++i)
    addr[i] = static_cast<std::uint8_t>(i + 1);
  ASSERT_TRUE(dao.write_address_balance(addr, int128_t{12345}).has_value());

  RPCContext ctx(std::move(dao), nullptr);

  AddressAndKeyBatch batch;
  batch.push_back(make_balance_query(addr));

  HogDbGatewayRpc gateway(ctx);

  // Hold a commit publication open, then release it from another thread once
  // the gateway has had a chance to observe it and block in the wait.
  auto publication_guard =
      std::make_unique<RPCContext::CommitPublicationGuard>(ctx, BlockTXID{1});

  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    publication_guard.reset(); // finish_commit_publication
  });

  DbGatewayResult result = decode(gateway.dbgateway_get_items(batch)());
  releaser.join();

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  EXPECT_EQ(from_xdr(result.payload().items[0].item().nnint()),
            int128_t{12345});
}

// A batch at the cap is serviced; one item over the cap is rejected
// wholesale before any reads (issue #4056).
TEST(HogDbGatewayRpcTest, GetItemsRejectsBatchesOverCap)
{
  HogDbDAO dao = make_dao();
  RPCContext ctx(std::move(dao), nullptr);
  HogDbGatewayRpc gateway(ctx);

  // Zero address: nothing is seeded, so each item is a cheap miss.
  ContractAddress addr{};

  AddressAndKeyBatch batch;
  for (std::size_t i = 0; i < HogDbGatewayRpc::kMaxBatchItems; ++i)
    batch.push_back(make_balance_query(addr));

  DbGatewayResult ok = decode(gateway.dbgateway_get_items(batch)());
  ASSERT_EQ(ok.error(), RPCError::kRPCNone);
  EXPECT_EQ(ok.payload().items.size(), HogDbGatewayRpc::kMaxBatchItems);

  // One more item exceeds the cap; the call above took its own copy.
  batch.push_back(make_balance_query(addr));
  DbGatewayResult rejected = decode(gateway.dbgateway_get_items(batch)());
  EXPECT_EQ(rejected.error(), RPCError::kRPCUnrecoverable);
}

// Bytestrings go out as the rows read (DbGatewayItemsReply), so check they
// reach the client intact on every padding case and at the ceiling, batched
// with an item that travels as the DbGatewayItemResult it is, and that the
// reply's bytes are exactly the DbGatewayResult's encoding.
TEST(HogDbGatewayRpcTest, GetItemsReturnsBytestringsIntact)
{
  HogDbDAO dao = make_dao();

  ContractAddress addr{};
  addr[0] = 0x44;
  ASSERT_TRUE(dao.write_address_balance(addr, int128_t{777}).has_value());

  constexpr std::array<std::size_t, 5> kLengths{0, 1, 3, 13,
                                                static_cast<std::size_t>(
                                                    kBytestringCeilingLen)};
  std::vector<std::vector<std::byte>> values;
  AddressAndKeyBatch batch;
  batch.push_back(make_balance_query(addr));
  for (std::size_t i = 0; i < kLengths.size(); ++i) {
    StorageKey storage_key(StorageKeyType::ContractData);
    storage_key.contract_data_key()[0] = static_cast<std::uint8_t>(i + 1);
    AddressAndKey query{.address = addr, .key = storage_key};
    std::vector<std::byte> value(kLengths[i]);
    for (std::size_t j = 0; j < value.size(); ++j)
      value[j] = static_cast<std::byte>((j * 7) + 1);
    ASSERT_TRUE(dao.write_bytes(make_item_key(query), value).has_value());
    values.push_back(std::move(value));
    batch.push_back(query);
  }

  RPCContext ctx(std::move(dao), nullptr);
  HogDbGatewayRpc gateway(ctx);
  DbGatewayItemsReply reply = gateway.dbgateway_get_items(batch)();
  x23::Buf wire = x23::serialize(reply);
  EXPECT_EQ(wire.size(), xdr_size(reply));
  DbGatewayResult result = decode(reply);
  EXPECT_TRUE(std::ranges::equal(x23::serialize(result), wire));

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), kLengths.size() + 1);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  EXPECT_EQ(result.payload().items[0].item().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  for (std::size_t i = 0; i < kLengths.size(); ++i) {
    DbGatewayItemResult const &item = result.payload().items[i + 1];
    ASSERT_EQ(item.error(), RPCError::kRPCNone);
    ASSERT_EQ(item.item().type(), StorageItemType::kItemTypeBytestring);
    EXPECT_TRUE(
        std::ranges::equal(std::as_bytes(std::span(item.item().bytestring())),
                           values[i]))
        << "length " << kLengths[i];
  }
}

// A stored row longer than any Bytestring is corrupt state: its item carries
// the error, not an opaque the client's decode would reject.
TEST(HogDbGatewayRpcTest, GetItemsReportsOversizedBytestringRow)
{
  HogDbDAO dao = make_dao();

  ContractAddress addr{};
  addr[0] = 0x55;
  StorageKey storage_key(StorageKeyType::ContractData);
  storage_key.contract_data_key()[0] = 1;
  AddressAndKey query{.address = addr, .key = storage_key};
  std::vector<std::byte> value(static_cast<std::size_t>(kBytestringCeilingLen) +
                                   4,
                               std::byte{0xCD});
  ASSERT_TRUE(dao.write_bytes(make_item_key(query), value).has_value());

  AddressAndKeyBatch batch;
  batch.push_back(query);

  RPCContext ctx(std::move(dao), nullptr);
  HogDbGatewayRpc gateway(ctx);
  DbGatewayItemsReply reply = gateway.dbgateway_get_items(batch)();
  ASSERT_EQ(reply.items().size(), 1U);
  EXPECT_EQ(reply.items()[0].bytestring(), nullptr);

  DbGatewayResult result = decode(reply);
  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  EXPECT_EQ(result.payload().items[0].error(), RPCError::kRPCUnrecoverable);
}

// Item count alone doesn't bound response size; an oversized response is
// rejected even when the item count stays under kMaxBatchItems (#4056).
TEST(HogDbGatewayRpcTest, GetItemsRejectsResponseOverByteBudget)
{
  HogDbDAO dao = make_dao();

  ContractAddress addr{};
  addr[0] = 0x33;
  StorageKey storage_key(StorageKeyType::ContractData);
  storage_key.contract_data_key()[0] = 9;
  AddressAndKey query{.address = addr, .key = storage_key};

  // One large bytestring, duplicated just past the byte budget but under
  // the count cap, so this exercises the byte budget, not the count cap.
  constexpr std::size_t kValueBytes = 60000;
  std::vector<std::byte> value(kValueBytes, std::byte{0xAB});
  ASSERT_TRUE(dao.write_bytes(make_item_key(query), value).has_value());

  const std::size_t count =
      (HogDbGatewayRpc::kMaxResponseBytes / kValueBytes) + 2;
  ASSERT_LT(count, HogDbGatewayRpc::kMaxBatchItems);

  AddressAndKeyBatch batch;
  for (std::size_t i = 0; i < count; ++i)
    batch.push_back(query);

  RPCContext ctx(std::move(dao), nullptr);
  HogDbGatewayRpc gateway(ctx);
  DbGatewayResult result = decode(gateway.dbgateway_get_items(batch)());
  EXPECT_EQ(result.error(), RPCError::kRPCUnrecoverable);
}

// Regression for #4060: dispatch owns the decoded argument only until serve
// returns. A handler suspended on commit publication must own its batch.
TEST(HogDbGatewayRpcTest, DispatchedGetItemsOwnsBatchAcrossCommitPublication)
{
  HogDbDAO dao = make_dao();
  ContractAddress first{};
  first[0] = 1;
  ContractAddress second{};
  second[0] = 2;
  ASSERT_TRUE(dao.write_address_balance(first, int128_t{12345}));
  ASSERT_TRUE(dao.write_address_balance(second, int128_t{67890}));

  RPCContext ctx(std::move(dao), nullptr);
  HogDbGatewayRpc gateway(ctx);
  x23::RpcDispatcher<DbGateway1, HogDbGatewayRpc *> dispatcher(&gateway, {});
  auto call = x23::call_msg<DbGateway1::proc_meta<1>>();
  call.xid = 4060;
  auto publication_guard =
      std::make_unique<RPCContext::CommitPublicationGuard>(ctx, BlockTXID{1});

  auto pending = [&] {
    AddressAndKeyBatch batch{make_balance_query(first),
                             make_balance_query(ContractAddress{}),
                             make_balance_query(second)};
    auto args = x23::serialize(batch);
    return dispatcher.serve(call, args);
  }();
  // The held publication forces suspension without sleeps or a timing race.
  EXPECT_FALSE(pending.await_ready());
  publication_guard.reset();
  auto reply = pending();

  ASSERT_GE(reply.size(), 4U);
  x23::rpc_msg msg;
  auto header_size =
      x23::deserialize_from(&msg, x23::BufView(reply).subspan(4));
  EXPECT_EQ(msg.xid, call.xid);
  ASSERT_EQ(msg.body.mtype(), x23::msg_type::REPLY);
  ASSERT_EQ(msg.body.rbody().stat(), x23::reply_stat::MSG_ACCEPTED);
  ASSERT_EQ(msg.body.rbody().areply().reply_data.stat(),
            x23::accept_stat::SUCCESS);
  DbGatewayResult result;
  x23::deserialize(&result, x23::BufView(reply).subspan(4 + header_size));
  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 3U);
  auto const &items = result.payload().items;
  ASSERT_EQ(items[0].error(), RPCError::kRPCNone);
  ASSERT_EQ(items[0].item().type(), StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(items[0].item().nnint()), int128_t{12345});
  EXPECT_EQ(items[1].error(), RPCError::kRPCNotFound);
  ASSERT_EQ(items[2].error(), RPCError::kRPCNone);
  ASSERT_EQ(items[2].item().type(), StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(items[2].item().nnint()), int128_t{67890});
}

// is_committed_read_stale must report a snapshot as stale once the tip has
// advanced past it, and as fresh for the current tip.
TEST(HogDbGatewayRpcTest, CommittedReadStaleDetectsTipAdvance)
{
  HogDbDAO dao = make_dao();
  RPCContext ctx(std::move(dao), nullptr);

  DbTip before = ctx.get_tip();
  EXPECT_FALSE(ctx.is_committed_read_stale(before));

  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  ASSERT_TRUE(
      ctx.update_dbtip(BlockTXID{7}, value, StorageLongHash{}).has_value());

  // The pre-advance snapshot is now stale; the current tip is not.
  EXPECT_TRUE(ctx.is_committed_read_stale(before));
  EXPECT_FALSE(ctx.is_committed_read_stale(ctx.get_tip()));
}

} // namespace hogd
