#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

// xdr types
#include "contract.hh"
#include "hogd/base/log.h"
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
#include "precompiles.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
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

  ASSERT_TRUE(rsize->second.is_cached_nnint128_base());
  EXPECT_EQ(0xABCD'EF01'2345'6789, rsize->second.get_cached_nnint128_base());
}

TEST_F(HogDBTest, ItemLockCacheDataBalance)
{
  ItemKey ikey = make_address_balance_item_key(make_address(kAddress));
  EXPECT_TRUE(db_.write_address_balance(ikey.get_address(), 100000));
  auto rbalance =
      db_.get_charged_storage_cost_and_cache_data(ikey, &cost_model_);
  ASSERT_TRUE(rbalance);
  EXPECT_EQ(0, rbalance->first);
  ASSERT_TRUE(rbalance->second.is_cached_balance());
  EXPECT_EQ(100000, rbalance->second.get_cached_balance().value());
}

TEST_F(HogDBTest, ItemLockCacheDataNoBalance)
{
  ItemKey ikey = make_address_balance_item_key(make_address(kAddress));
  HogDBStatus<std::pair<int128_t, ItemLockCacheData>> rbalance =
      db_.get_charged_storage_cost_and_cache_data(ikey, &cost_model_);
  ASSERT_TRUE(rbalance);
  EXPECT_EQ(0, rbalance->first);
  ASSERT_TRUE(rbalance->second.is_cached_balance());
  EXPECT_FALSE(rbalance->second.get_cached_balance().has_value());
}

TEST_F(HogDBTest, HogAgeWrittenOnDeployment)
{
  Address addr = make_address(kAddress);
  Address addr2 = make_address(kAddress + 1);
  Address addr3 = make_address(kAddress + 2);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(1);

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
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(addr2), 100000,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));

  ASSERT_TRUE(invoke_tx.request_deploy_contract(
      addr3,
      DeployedContractHash{.hash = sha256(contract), .contract_size = 10}));
  int128_t cost =
      cost_model_.get_contract_size_cost(10) + cost_model_.get_balance_cost();
  CHECK(cost < 100000); // for the test, given the above constant
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(addr3), -cost,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(addr3), cost,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));

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
    LockContext ctx(&db_, height, rckey, &cost_model_);
    auto r = lock.try_atomic_change_lock(nullptr, &c1, ctx);
    ASSERT_TRUE(r);
    auto commit_cost = lock.commit_change(c1, ctx);
    EXPECT_EQ(cost_model_.kBaseItemCost + cost_model_.kReplayCacheCostPerEntry,
              commit_cost);
    ASSERT_TRUE(lock.write_committed_change(ctx, trie_));
  }

  height++;

  // Read the replay cache metadata and data that was written in the previous
  // block
  auto meta = db_.read_address_replay_cache_meta(rckey.get_address());
  ASSERT_TRUE(meta);

  // Verify metadata
  ASSERT_EQ(1, meta->size);
  ASSERT_EQ(1, meta->heights.size());
  EXPECT_EQ(height, meta->heights[0].height);
  EXPECT_EQ(1, meta->heights[0].nonce_count);

  // Read data for height 10
  auto data_h10 =
      db_.read_address_replay_cache_data(rckey.get_address(), height);
  ASSERT_TRUE(data_h10);
  ASSERT_EQ(1, data_h10->nonces.size());
  EXPECT_EQ(101, data_h10->nonces[0]);

  // Create a new lock with the snapshot cost from the database
  auto snapshot_result =
      db_.get_charged_storage_cost_and_cache_data(rckey, &cost_model_);
  ASSERT_TRUE(snapshot_result);
  ItemLock lock2(snapshot_result->first);

  {
    LockContext ctx(&db_, height, rckey, &cost_model_);

    // height is now 10, 101 expired, and it shouldn't be possible to readd it
    ItemChange c2(ReplayCacheIt(Uniquifier(height, 101)));
    auto r = lock2.try_atomic_change_lock(nullptr, &c2, ctx);
    ASSERT_FALSE(r);

    ItemChange c3(ReplayCacheIt(Uniquifier(height + 1, 102)));
    auto r2 = lock2.try_atomic_change_lock(nullptr, &c3, ctx);
    ASSERT_TRUE(r2);
    EXPECT_EQ(cost_model_.kReplayCacheCostPerEntry, r2.value());

    auto commit_cost = lock2.commit_change(c3, ctx);
    EXPECT_EQ(cost_model_.kReplayCacheCostPerEntry, commit_cost);
    ASSERT_TRUE(lock2.write_committed_change(ctx, trie_));
  }

  // Read final metadata and data
  auto final_meta = db_.read_address_replay_cache_meta(rckey.get_address());
  ASSERT_TRUE(final_meta);

  // Verify final metadata - should have 2 total nonces across 2 heights
  ASSERT_EQ(2, final_meta->size);
  ASSERT_EQ(2, final_meta->heights.size());
  EXPECT_EQ(height, final_meta->heights[0].height);
  EXPECT_EQ(1, final_meta->heights[0].nonce_count);
  EXPECT_EQ(height + 1, final_meta->heights[1].height);
  EXPECT_EQ(1, final_meta->heights[1].nonce_count);

  // Verify data at height 10
  auto final_data_h10 =
      db_.read_address_replay_cache_data(rckey.get_address(), height);
  ASSERT_TRUE(final_data_h10);
  ASSERT_EQ(1, final_data_h10->nonces.size());
  EXPECT_EQ(101, final_data_h10->nonces[0]);

  // Verify data at height 11
  auto final_data_h11 =
      db_.read_address_replay_cache_data(rckey.get_address(), height + 1);
  ASSERT_TRUE(final_data_h11);
  ASSERT_EQ(1, final_data_h11->nonces.size());
  EXPECT_EQ(102, final_data_h11->nonces[0]);
}

TEST_F(HogDBTest, BalanceTransferReducesByCost)
{
  Address sender_addr = make_address(kAddress);
  Address recipient_addr = make_address(kAddress + 1);

  // Set initial balance for sender
  constexpr int128_t kInitialBalance = 100000;
  ASSERT_TRUE(db_.write_address_balance(sender_addr, kInitialBalance));

  db_.load_trie(trie_);

  BlockHeight height = 1;
  PerThreadARC<BlockTX> block_tx(db_, trie_, cost_model_,
                                 background_commit_worker_, height);

  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Transfer balance to a new address
  constexpr int128_t kTransferAmount = 50000;
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(sender_addr), -kTransferAmount,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));
  ASSERT_TRUE(invoke_tx.request_nnint128(
      make_address_balance_item_key(recipient_addr), kTransferAmount,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));

  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  ASSERT_TRUE(block_tx.get().commit());

  // Read back the sender's balance
  auto sender_balance = db_.read_address_balance(sender_addr);
  ASSERT_TRUE(sender_balance);

  // The balance should be reduced by the transfer amount
  // (cost for this elt is implicit in the fact that the balance exists in the
  // db snapshot that was loaded at the start of the test)
  int128_t expected_sender_balance = kInitialBalance - kTransferAmount;
  EXPECT_EQ(expected_sender_balance, *sender_balance);

  // Verify recipient received the transfer amount
  auto recipient_balance = db_.read_address_balance(recipient_addr);
  ASSERT_TRUE(recipient_balance);
  EXPECT_EQ(kTransferAmount - cost_model_.get_balance_cost(),
            *recipient_balance);
}

TEST_F(HogDBTest, TwoTransfersToSameAddressCostOnce)
{
  Address sender_addr = make_address(kAddress);
  Address recipient_addr = make_address(kAddress + 1);

  // Set initial balance for sender
  constexpr int128_t kInitialBalance = 200000;
  constexpr static int128_t kFirstTransferAmount = 50000;
  constexpr static int128_t kSecondTransferAmount = 30000;

  ASSERT_TRUE(db_.write_address_balance(sender_addr, kInitialBalance));

  db_.load_trie(trie_);

  BlockHeight height = 1;
  PerThreadARC<BlockTX> block_tx(db_, trie_, cost_model_,
                                 background_commit_worker_, height);

  {
    // First InvokeTX: transfer to recipient (new address, cost applies)
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx1(block_tx);

    ASSERT_TRUE(invoke_tx1.request_nnint128(
        make_address_balance_item_key(sender_addr), -kFirstTransferAmount,
        std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));
    ASSERT_TRUE(invoke_tx1.request_nnint128(
        make_address_balance_item_key(recipient_addr), kFirstTransferAmount,
        std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));

    ASSERT_TRUE(invoke_tx1.sync_balance_locks());
    invoke_tx1.commit();
    invoke_tx1.release_blocktx_reference();
  }

  {
    // Second InvokeTX: transfer to same recipient (existing address, no cost)
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx2(block_tx);

    ASSERT_TRUE(invoke_tx2.request_nnint128(
        make_address_balance_item_key(sender_addr), -kSecondTransferAmount,
        std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));
    ASSERT_TRUE(invoke_tx2.request_nnint128(
        make_address_balance_item_key(recipient_addr), kSecondTransferAmount,
        std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone)));

    ASSERT_TRUE(invoke_tx2.sync_balance_locks());
    invoke_tx2.commit();
    invoke_tx2.release_blocktx_reference();
  }

  // Commit the block transaction
  ASSERT_TRUE(block_tx.get().commit());

  // Verify sender's balance
  auto sender_balance = db_.read_address_balance(sender_addr);
  ASSERT_TRUE(sender_balance);
  int128_t expected_sender_balance =
      kInitialBalance - kFirstTransferAmount - kSecondTransferAmount;
  EXPECT_EQ(expected_sender_balance, *sender_balance);

  // Verify recipient's final balance: sum of both transfers minus cost only
  // once
  auto recipient_balance = db_.read_address_balance(recipient_addr);
  ASSERT_TRUE(recipient_balance);
  int128_t expected_recipient_balance = kFirstTransferAmount +
                                        kSecondTransferAmount -
                                        cost_model_.get_balance_cost();
  EXPECT_EQ(expected_recipient_balance, *recipient_balance);
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
