#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <utility>

#include "db_sched.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_cost.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

HogAge
make_hog_age(uint128_t num_hogs, BlockHeight average_age)
{
  auto num_hogs_xdr = to_xdr(num_hogs);
  return HogAge{.address = ContractAddress{},
                .num_hogs = {{num_hogs_xdr, num_hogs_xdr, num_hogs_xdr}},
                .average_ages = {{average_age, average_age, average_age}}};
}

} // namespace

TEST(RecoveryTests, RecoveryVaryingItems)
{

  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;

  ASSERT_TRUE(dir.has_value());
  DbTip initial_tip{};
  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    ASSERT_TRUE(db);
    auto dao = HogDBDAO(RAMDB(std::move(*db)));
    MerkleTriePrefix trie;
    dao.load_trie(trie);
    EXPECT_EQ(trie.size(), 0UL);

    // add some items to the database
    // some items that are not in the trie
    CHECK(dao.write_address_average_hog_age(make_hog_age(0U, 4)).has_value());
    // some items that are in the trie

    RPCContext ctx(dao, trie, nullptr);
    ctx.initialize_cost_model(StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = to_xdr(int128_t{12000}),
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = to_xdr(int128_t{12000}),
        .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
    }));
    ASSERT_TRUE(
        ctx.get_db().write_cost_model(ctx.get_cost_model()).has_value());
    auto btx = ctx.open_block_tx(1);
    ASSERT_TRUE(btx);

    auto *itx = ctx.get_invoke_tx(1, 1);

    ASSERT_TRUE(itx);
    auto sp = itx->create_savepoint();
    auto cleanup =
        x23::Defer([itx, sp]() { itx->rollback_to_savepoint(sp - 1); });

    ASSERT_TRUE(
        itx->request_nnint128(make_address_balance_item_key(ContractAddress{0}),
                              ctx.get_cost_model().get_nnint_cost() +
                                  ctx.get_cost_model().get_bytestring_cost(6))
            .has_value());
    ASSERT_TRUE(itx->request_nnint128(ItemKey(ContractAddress{0},
                                              StorageKeyContractData{0}),
                                      10)
                    .has_value());
    ASSERT_TRUE(itx->request_bytes(ItemKey(ContractAddress{0},
                                           StorageKeyContractData{1}),
                                   std::as_bytes(std::span("hello")))
                    .has_value());

    // invoketx assertions fail if you call the dtor in an invalid state (where
    // i.e requests failed).
    cleanup.release();

    itx->commit();
    ASSERT_TRUE((*btx)->commit().has_value());
    ctx.close_block_tx(1);

    ASSERT_TRUE(ctx.update_dbtip(1).has_value());

    // commit to disk
    auto lsn = ctx.get_db().commit();
    ASSERT_TRUE(lsn.has_value());
    EXPECT_EQ(*lsn, 1UL);

    initial_tip = ctx.get_tip();
    EXPECT_EQ(ctx.get_trie().size(), 3UL);
  }

  // now reload the database
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  ASSERT_TRUE(db);
  auto dao = HogDBDAO(RAMDB(std::move(*db)));
  MerkleTriePrefix trie;
  dao.load_trie(trie);
  EXPECT_EQ(trie.size(), 3UL);
  ASSERT_EQ(trie.get_root_hash(), initial_tip.hash);

  ASSERT_TRUE(dao.read_cost_model().has_value());
}

} // namespace hogd
