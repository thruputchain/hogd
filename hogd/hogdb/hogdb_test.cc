#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

// xdr types
#include "account.hh"
#include "contract.hh"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_cost.hh"
#include "storage_delta_flags.hh"
#include "transaction.hh"
#include "types.hh"

#include "hogd/base/io.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

class HogDBTest : public testing::Test {
protected:
  HogDBTest() = default;
  ~HogDBTest() override = default;

  void SetUp() override
  {
    ErrnoStatus<Directory> dir = Directory::open_temporary();
    ASSERT_TRUE(static_cast<bool>(dir));

    constexpr size_t kHeapSize = 10_MiB;
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), Database::estimate_heap_size(kHeapSize),
                       0);
    ASSERT_TRUE(db);
    db_ = HogDBDAO(RAMDB(std::move(*db)));
  }

  HogDBDAO db_;
  MerkleTriePrefix trie_;

  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = to_xdr(int128_t{12000}),
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = to_xdr(int128_t{12000}),
      .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
      .replay_cache_cost_per_entry = to_xdr(int128_t{7}),
  });

  AuthChangeSubscriptionManager subscription_manager_;
  BackgroundCommitWorker background_commit_worker_ =
      BackgroundCommitWorker(subscription_manager_, nullptr);
};

namespace {

constexpr ItemKey
make_key(size_t addr, size_t item)
{
  return make_item_key(make_address(addr), item);
}

constexpr uint64_t kAddress = 0xdeadbeef;

} // namespace

TEST_F(HogDBTest, DataAccessObject)
{
  //
  // Global operations
  //

  // test db tip
  DbTip tip{.height = 10,
            .hash = sha256(std::as_bytes(std::span("foo"))),
            .id = 30};
  EXPECT_TRUE(db_.write_dbtip(tip));
  HogDBStatus<DbTip> rtip = db_.read_dbtip();
  EXPECT_TRUE(rtip);
  if (rtip) {
    EXPECT_EQ(*rtip, tip);
  }

  //
  // Address operations
  //
  Address addr = make_address(kAddress);

  // test address balance
  EXPECT_TRUE(db_.write_address_balance(addr, 10));
  HogDBStatus<int128_t> bal = db_.read_address_balance(addr);
  EXPECT_TRUE(bal);
  if (bal) {
    EXPECT_EQ(*bal, 10);
  }

  // test wasm hash
  std::string test_code("wasm contract code is here");
  uint32_t contract_size = test_code.size();
  SHA256Hash hash = sha256(std::as_bytes(std::span(test_code)));
  EXPECT_TRUE(db_.write_address_metadata(
      addr,
      DeployedContractHash{.hash = hash, .contract_size = contract_size}));
  auto rhash = db_.read_address_metadata(addr);
  EXPECT_TRUE(rhash);
  if (rhash) {
    EXPECT_EQ(hash, rhash->hash);
  }

  // test wasm code
  EXPECT_TRUE(db_.write_wasm_code(hash, std::as_bytes(std::span(test_code))));
  HogDBStatus<OwningSpan<const std::byte>> rcode = db_.read_wasm_code(hash);
  EXPECT_TRUE(rcode);
  if (rcode) {
    EXPECT_EQ(std::as_bytes(std::span(test_code)), *rcode);
  }

  //
  // Item operations
  //
  ItemKey ikey = make_key(kAddress, kAddress);

  // test nnint128 item
  int128_t test_nnint128 = 10;
  EXPECT_TRUE(db_.write_nnint128(ikey, test_nnint128));
  HogDBStatus<int128_t> rnnint128 = db_.read_nnint128(ikey);
  EXPECT_TRUE(rnnint128);
  if (rnnint128) {
    EXPECT_EQ(test_nnint128, *rnnint128);
  }
  HogDBStatus<std::pair<int128_t, ItemLockCacheData>> rsize =
      db_.get_charged_storage_cost_and_cache_data(ikey, &cost_model_);
  EXPECT_TRUE(rsize);
  if (rsize) {
    EXPECT_EQ(cost_model_.get_nnint_cost(), rsize->first);
  }
  HogDBStatus<uint8_t> rtype = db_.get_item_type(ikey);
  EXPECT_TRUE(rtype);
  if (rtype) {
    EXPECT_EQ(kNNInt128, *rtype);
  }
  EXPECT_TRUE(db_.remove_item(ikey));

  // test bytes item
  std::string test_bytes("bytes data is here");
  EXPECT_TRUE(db_.write_bytes(ikey, std::as_bytes(std::span(test_bytes))));
  HogDBStatus<OwningSpan<const std::byte>> rbytes = db_.read_bytes(ikey);
  EXPECT_TRUE(rbytes);
  if (rbytes) {
    EXPECT_EQ(std::as_bytes(std::span(test_bytes)), *rbytes);
  }
  rsize = db_.get_charged_storage_cost_and_cache_data(ikey, &cost_model_);
  EXPECT_TRUE(rsize);
  if (rsize) {
    EXPECT_EQ(cost_model_.get_bytestring_cost(test_bytes.size()), rsize->first);
  }
  rtype = db_.get_item_type(ikey);
  EXPECT_TRUE(rtype);
  if (rtype) {
    EXPECT_EQ(kBytes, *rtype);
  }
  EXPECT_TRUE(db_.remove_item(ikey));
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_F(HogDBTest, ItemLockCacheData)
{
  ItemKey ikey = make_key(kAddress, kAddress);
  EXPECT_TRUE(db_.write_nnint128(ikey, 0xABCD'EF01'2345'6789));
  HogDBStatus<int128_t> rnnint128 = db_.read_nnint128(ikey);
  ASSERT_TRUE(rnnint128);
  EXPECT_EQ(0xABCD'EF01'2345'6789, *rnnint128);

  HogDBStatus<std::pair<int128_t, ItemLockCacheData>> rsize =
      db_.get_charged_storage_cost_and_cache_data(ikey, &cost_model_);
  ASSERT_TRUE(rsize);
  EXPECT_EQ(cost_model_.get_nnint_cost(), rsize->first);
  int128_t base;
  std::memcpy(&base, rsize->second.data() + 1, sizeof(int128_t));
  EXPECT_EQ(0xABCD'EF01'2345'6789, base);
}

TEST_F(HogDBTest, HogAgeWrittenOnDeployment)
{
  Address addr = make_address(kAddress);
  Address addr2 = make_address(kAddress + 1);
  Address addr3 = make_address(kAddress + 2);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 1;

  ASSERT_TRUE(
      db_.write_address_metadata(addr,
                                 DeployedContractHash{.hash = sha256(contract),
                                                      .contract_size = 10}));
  ASSERT_TRUE(db_.write_address_balance(addr, 1000000));

  db_.load_trie(trie_);

  BlockHeight height = 1;
  PerThreadARC<BlockTX> block_tx(db_, trie_, cost_model_,
                                 background_commit_worker_, height);

  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  ASSERT_TRUE(invoke_tx.request_deploy_contract(
      addr2,
      DeployedContractHash{.hash = sha256(contract), .contract_size = 10}));
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(addr), -100000,
      std::to_underlying(StorageItemRPCFlags::kStorageNone)));
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(addr2), 100000,
      std::to_underlying(StorageItemRPCFlags::kStorageNone)));

  ASSERT_TRUE(invoke_tx.request_deploy_contract(
      addr3,
      DeployedContractHash{.hash = sha256(contract), .contract_size = 10}));
  int128_t cost = cost_model_.get_contract_size_cost(10);
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(addr3), -cost,
      std::to_underlying(StorageItemRPCFlags::kStorageNone)));
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(addr3), cost,
      std::to_underlying(StorageItemRPCFlags::kStorageNone)));

  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  ASSERT_TRUE(block_tx.get().commit());

  auto res = db_.read_address_balance(addr3);
  ASSERT_TRUE(res);
  EXPECT_EQ(0, *res);

  // Creating the new contract should force the creation of the hog age entry
  // for the address.
  ASSERT_TRUE(db_.read_address_average_hog_age(addr2));
  ASSERT_TRUE(db_.read_address_average_hog_age(addr3));
}
// NOLINTEND(bugprone-unchecked-optional-access)

TEST_F(HogDBTest, ItemChangeReplayCache)
{
  BlockHeight height = 9;
  auto rckey = make_replay_cache_item_key(make_address("abcd"));

  // Create a replay cache item wrapped in ItemChange
  ItemChange c1(ReplayCacheIt(Uniquifier(height + 1, 101)));

  // Create ItemLock with snapshot cost of 0 (no existing data)
  ItemLock lock(0);

  {
    LockContext ctx(&db_, height, rckey);
    auto r = lock.try_atomic_change_lock(nullptr, &c1, &cost_model_, ctx);
    ASSERT_TRUE(r);
    auto commit_cost = lock.commit_change(c1, &cost_model_, ctx);
    EXPECT_EQ(cost_model_.kBaseItemCost + cost_model_.kReplayCacheCostPerEntry,
              commit_cost);
    ASSERT_TRUE(lock.write_committed_change(ctx, trie_));
  }

  height++;

  // Read the replay cache data that was written in the previous block
  auto new_base = db_.read_address_replay_cache(rckey.get_address());
  ASSERT_TRUE(new_base);

  // Verify that the data was persisted correctly
  ASSERT_EQ(1, new_base->uniquifiers.size());
  EXPECT_EQ(height, new_base->uniquifiers[0].block_expiry_height);
  EXPECT_EQ(101, new_base->uniquifiers[0].nonce);

  // Create a new lock with the snapshot cost from the database
  auto snapshot_result =
      db_.get_charged_storage_cost_and_cache_data(rckey, &cost_model_);
  ASSERT_TRUE(snapshot_result);
  ItemLock lock2(snapshot_result->first);

  {
    LockContext ctx(&db_, height, rckey);

    // height is now 10, 101 expired, and it shouldn't be possible to readd it
    ItemChange c2(ReplayCacheIt(Uniquifier(height, 101)));
    auto r = lock2.try_atomic_change_lock(nullptr, &c2, &cost_model_, ctx);
    ASSERT_FALSE(r);

    ItemChange c3(ReplayCacheIt(Uniquifier(height + 1, 102)));
    auto r2 = lock2.try_atomic_change_lock(nullptr, &c3, &cost_model_, ctx);
    ASSERT_TRUE(r2);
    EXPECT_EQ(cost_model_.kReplayCacheCostPerEntry, r2.value());

    auto commit_cost = lock2.commit_change(c3, &cost_model_, ctx);
    EXPECT_EQ(cost_model_.kReplayCacheCostPerEntry, commit_cost);
    ASSERT_TRUE(lock2.write_committed_change(ctx, trie_));
  }

  auto final_base = db_.read_address_replay_cache(rckey.get_address());
  ASSERT_TRUE(final_base);

  // Verify that the data was persisted correctly
  ASSERT_EQ(2, final_base->uniquifiers.size());
  EXPECT_EQ(height, final_base->uniquifiers[0].block_expiry_height);
  EXPECT_EQ(101, final_base->uniquifiers[0].nonce);
  EXPECT_EQ(height + 1, final_base->uniquifiers[1].block_expiry_height);
  EXPECT_EQ(102, final_base->uniquifiers[1].nonce);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv, false);
  if (ret)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
