// Smoke tests for HogDbGatewayRpc.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

// xdr types
#include "consensus.hh"
#include "db_gateway.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/io.h"
#include "hogd/base/log.h"
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
  DbGatewayResult result = gateway.dbgateway_get_items(batch);

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items[0].item().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(result.payload().items[0].item().nnint()),
            int128_t{12345});
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
  DbGatewayResult result = gateway.dbgateway_get_items(batch);

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
        BlockExecutionMode::kBlockExecutionModeProposal);
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
  DbGatewayResult result = gateway.dbgateway_get_items(batch);

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items[0].item().type(),
            StorageItemType::kItemTypeNonnegativeInt);

  int128_t gateway_value = from_xdr(result.payload().items[0].item().nnint());
  EXPECT_EQ(gateway_value, int128_t{1});
}

// Exercises the wait-out-publication path. While a commit is mid-publication
// (commit_publishing_ held), a committed-tip read blocks in
// wait_for_no_commit_publishing() instead of reading a tip that is
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

  DbGatewayResult result = gateway.dbgateway_get_items(batch);
  releaser.join();

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  EXPECT_EQ(from_xdr(result.payload().items[0].item().nnint()),
            int128_t{12345});
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
