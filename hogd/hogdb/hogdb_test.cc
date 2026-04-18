#include <algorithm>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

// xdr types
#include "address_auth.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "precompiles.hh"

#include "hogd/base/io.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_age_reconciliation.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

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

  template<typename F>
  void commit_block_expect_db_hash_matches_full(BlockHeight height, F &&fn)
  {
    DbHash base_hash = db_.compute_full_db_hash();

    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    fn(invoke_tx);

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    DbHash incremental_hash = *hash_result;
    ASSERT_TRUE(block_tx.get().commit());

    DbHash full_hash = db_.compute_full_db_hash();
    EXPECT_EQ(incremental_hash, full_hash);
  }

  template<typename F>
  DbHash commit_block_and_return_db_hash(BlockHeight height, F &&fn)
  {
    DbHash base_hash = db_.compute_full_db_hash();

    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    bool acquired_reference = block_tx.try_acquire_reference();
    EXPECT_TRUE(acquired_reference);
    if (!acquired_reference) {
      return {};
    }
    InvokeTX invoke_tx(block_tx);

    fn(invoke_tx);

    bool synced_balances = static_cast<bool>(invoke_tx.sync_balance_locks());
    EXPECT_TRUE(synced_balances);
    if (!synced_balances) {
      return {};
    }
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();

    auto hash_result = block_tx.get().close(base_hash);
    EXPECT_TRUE(hash_result.has_value());
    if (!hash_result.has_value()) {
      return {};
    }
    DbHash incremental_hash = *hash_result;
    bool committed = static_cast<bool>(block_tx.get().commit());
    EXPECT_TRUE(committed);
    if (!committed) {
      return {};
    }
    EXPECT_EQ(incremental_hash, db_.compute_full_db_hash());
    return incremental_hash;
  }

  void setup_contract_address(const Address &addr)
  {
    ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));
    ASSERT_TRUE(db_.write_address_metadata(
        addr,
        DeployedContractHash{.hash = sha256(std::as_bytes(std::span("seed"))),
                             .contract_size = 10}));
  }
};

namespace {

constexpr ItemKey
make_key(size_t addr, size_t item)
{
  return make_item_key(make_address(addr), item);
}

std::array<std::byte, sizeof(int128_t)>
raw_int128_le_bytes(int128_t value)
{
  int128_t le = cpu_to_le(value);
  std::array<std::byte, sizeof(int128_t)> out{};
  std::memcpy(out.data(), &le, out.size());
  return out;
}

constexpr uint64_t kAddress = 0xdeadbeef;

HogAge
make_hog_age(const Address &addr, uint128_t to_schedule,
             uint128_t already_scheduled, uint128_t received,
             BlockHeight to_schedule_age, BlockHeight already_scheduled_age,
             BlockHeight received_age)
{
  return HogAge{
      .address = addr.get_raw(),
      .num_hogs = {to_xdr(to_schedule), to_xdr(already_scheduled),
                   to_xdr(received)},
      .average_ages = {to_schedule_age, already_scheduled_age, received_age},
  };
}

} // namespace

TEST_F(HogDBTest, DataAccessObject)
{
  //
  // Global operations
  //

  // test db tip
  DbTip tip{.height = 10,
            .block_hash = sha256(std::as_bytes(std::span("foo"))),
            .db_hash = db_hash_from_buf(std::as_bytes(std::span("foo"))),
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

TEST_F(HogDBTest, NNIntAndBalancePersistenceUseCanonicalXdrBytes)
{
  constexpr int128_t kNNIntValue =
      (int128_t{0x0102030405060708LL} << 64) | 0x1112131415161718ULL;
  ItemKey item_key = make_key(kAddress + 20, 7);

  ASSERT_TRUE(db_.write_nnint128(item_key, kNNIntValue));

  auto raw_item = db_.read_contract_data_item(item_key);
  ASSERT_TRUE(raw_item);
  ASSERT_EQ(raw_item->type, kNNInt128);
  ASSERT_EQ(raw_item->value.size(), sizeof(int128_xdr));

  auto canonical_item_bytes = int128_to_xdr_bytes(kNNIntValue);
  auto raw_item_bytes = raw_int128_le_bytes(kNNIntValue);
  EXPECT_TRUE(std::equal(raw_item->value.begin(), raw_item->value.end(),
                         canonical_item_bytes.begin()));
  EXPECT_FALSE(std::equal(raw_item->value.begin(), raw_item->value.end(),
                          raw_item_bytes.begin()));

  auto reread_item = db_.read_nnint128(item_key);
  ASSERT_TRUE(reread_item);
  EXPECT_EQ(*reread_item, kNNIntValue);

  auto cached_item =
      db_.get_charged_storage_cost_and_cache_data(item_key, &cost_model_);
  ASSERT_TRUE(cached_item);
  EXPECT_EQ(cached_item->first, cost_model_.get_nnint_cost());
  ASSERT_TRUE(cached_item->second.is_cached_nnint128_base());
  EXPECT_EQ(cached_item->second.get_cached_nnint128_base(), kNNIntValue);

  const Address kBalanceAddr = make_address(kAddress + 21);
  constexpr int128_t kBalanceValue =
      (int128_t{0x2122232425262728LL} << 64) | 0x3132333435363738ULL;

  ASSERT_TRUE(db_.write_address_balance(kBalanceAddr, kBalanceValue));

  auto balance_key = make_address_key(kBalanceAddr, Prefix::kBalance);
  auto raw_balance =
      db_.get_ramdb().get_if_type(std::as_bytes(std::span(balance_key)),
                                  kNNInt128);
  ASSERT_TRUE(raw_balance);
  ASSERT_EQ(raw_balance->size(), sizeof(int128_xdr));

  auto canonical_balance_bytes = int128_to_xdr_bytes(kBalanceValue);
  auto raw_balance_bytes = raw_int128_le_bytes(kBalanceValue);
  EXPECT_TRUE(std::equal(raw_balance->begin(), raw_balance->end(),
                         canonical_balance_bytes.begin()));
  EXPECT_FALSE(std::equal(raw_balance->begin(), raw_balance->end(),
                          raw_balance_bytes.begin()));

  auto reread_balance = db_.read_address_balance(kBalanceAddr);
  ASSERT_TRUE(reread_balance);
  EXPECT_EQ(*reread_balance, kBalanceValue);
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

  BlockHeight height = 1;
  PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                 height);

  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  ASSERT_TRUE(invoke_tx.request_deploy_contract(
      addr2,
      DeployedContractHash{.hash = sha256(contract), .contract_size = 10}));
  ASSERT_TRUE(
      invoke_tx.request_nnint128(make_address_balance_item_key(addr), -100000,
                                 std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(
      invoke_tx.request_nnint128(make_address_balance_item_key(addr2), 100000,
                                 std::to_underlying(StorageDeltaFlags::None)));

  DeployedContractHash deployed_hash{.hash = sha256(contract),
                                     .contract_size = 10};
  ASSERT_TRUE(invoke_tx.request_deploy_contract(addr3, deployed_hash));
  int128_t cost = cost_model_.get_deployment_cost(deployed_hash) +
                  cost_model_.get_balance_cost();
  CHECK(cost < 100000); // for the test, given the above constant
  ASSERT_TRUE(
      invoke_tx.request_nnint128(make_address_balance_item_key(addr3), -cost,
                                 std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(
      invoke_tx.request_nnint128(make_address_balance_item_key(addr3), cost,
                                 std::to_underlying(StorageDeltaFlags::None)));

  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  ASSERT_TRUE(block_tx.get().close(DbHash{}));
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
    auto changes = db_.create_changeset();
    ASSERT_TRUE(lock.stage_committed_changes(ctx, changes));
    ASSERT_TRUE(db_.apply(changes));
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
    auto changes = db_.create_changeset();
    ASSERT_TRUE(lock2.stage_committed_changes(ctx, changes));
    ASSERT_TRUE(db_.apply(changes));
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

  BlockHeight height = 1;
  PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                 height);

  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Transfer balance to a new address
  constexpr int128_t kTransferAmount = 50000;
  ASSERT_TRUE(
      invoke_tx.request_nnint128(make_address_balance_item_key(sender_addr),
                                 -kTransferAmount,
                                 std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(
      invoke_tx.request_nnint128(make_address_balance_item_key(recipient_addr),
                                 kTransferAmount,
                                 std::to_underlying(StorageDeltaFlags::None)));

  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  ASSERT_TRUE(block_tx.get().close(DbHash{}));
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

TEST_F(HogDBTest, StartupHogAgeReconciliationAddsDeficitToToSchedule)
{
  Address account = make_address(kAddress);
  ASSERT_TRUE(db_.write_address_balance(account, 15));
  ASSERT_TRUE(db_.write_address_average_hog_age(
      make_hog_age(account, /*to_schedule=*/4,
                   /*already_scheduled=*/3,
                   /*received=*/2,
                   /*to_schedule_age=*/10,
                   /*already_scheduled_age=*/20,
                   /*received_age=*/30)));

  ASSERT_TRUE(reconcile_startup_hog_ages(db_));

  HogDBStatus<HogAge> age = db_.read_address_average_hog_age(account);
  ASSERT_TRUE(age);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 10U);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), 3U);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]), 2U);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 10);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 20);
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], 30);
}

TEST_F(HogDBTest, StartupHogAgeReconciliationDrainsReceivedBeforeOtherGroups)
{
  Address account = make_address(kAddress);
  ASSERT_TRUE(db_.write_address_balance(account, 6));
  ASSERT_TRUE(db_.write_address_average_hog_age(
      make_hog_age(account, /*to_schedule=*/4,
                   /*already_scheduled=*/3,
                   /*received=*/5,
                   /*to_schedule_age=*/10,
                   /*already_scheduled_age=*/20,
                   /*received_age=*/30)));

  ASSERT_TRUE(reconcile_startup_hog_ages(db_));

  HogDBStatus<HogAge> age = db_.read_address_average_hog_age(account);
  ASSERT_TRUE(age);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]), 0U);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), 2U);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 4U);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 10);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 20);
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], 30);
}

TEST_F(HogDBTest, StartupHogAgeReconciliationDrainsAllGroupsSequentially)
{
  Address account = make_address(kAddress);
  ASSERT_TRUE(db_.write_address_balance(account, 1));
  ASSERT_TRUE(db_.write_address_average_hog_age(
      make_hog_age(account, /*to_schedule=*/4,
                   /*already_scheduled=*/3,
                   /*received=*/2,
                   /*to_schedule_age=*/10,
                   /*already_scheduled_age=*/20,
                   /*received_age=*/30)));

  ASSERT_TRUE(reconcile_startup_hog_ages(db_));

  HogDBStatus<HogAge> age = db_.read_address_average_hog_age(account);
  ASSERT_TRUE(age);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]), 0U);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), 0U);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 1U);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 10);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 20);
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], 30);
}

TEST_F(HogDBTest, StartupHogAgeReconciliationFailsWhenBalanceRowLacksHogAge)
{
  Address account = make_address(kAddress);
  ASSERT_TRUE(db_.write_address_balance(account, 10));

  HogDBStatus<void> status = reconcile_startup_hog_ages(db_);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error(), HogDBCode::kNotFound);
}

TEST_F(HogDBTest, StartupHogAgeReconciliationFailsWhenHogAgeRowLacksBalance)
{
  Address account = make_address(kAddress);
  ASSERT_TRUE(db_.write_address_average_hog_age(
      make_hog_age(account, /*to_schedule=*/4,
                   /*already_scheduled=*/3,
                   /*received=*/2,
                   /*to_schedule_age=*/10,
                   /*already_scheduled_age=*/20,
                   /*received_age=*/30)));

  HogDBStatus<void> status = reconcile_startup_hog_ages(db_);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error(), HogDBCode::kNotFound);
}

TEST_F(HogDBTest, StartupHogAgeReconciliationFailsWhenBalanceIsNegative)
{
  Address account = make_address(kAddress);
  ASSERT_TRUE(db_.write_address_balance(account, -1));
  ASSERT_TRUE(db_.write_address_average_hog_age(
      make_hog_age(account, /*to_schedule=*/0,
                   /*already_scheduled=*/0,
                   /*received=*/0,
                   /*to_schedule_age=*/10,
                   /*already_scheduled_age=*/20,
                   /*received_age=*/30)));

  HogDBStatus<void> status = reconcile_startup_hog_ages(db_);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error(), HogDBCode::kInternal);
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

  BlockHeight height = 1;
  PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                 height);

  {
    // First InvokeTX: transfer to recipient (new address, cost applies)
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx1(block_tx);

    ASSERT_TRUE(invoke_tx1.request_nnint128(
        make_address_balance_item_key(sender_addr), -kFirstTransferAmount,
        std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx1.request_nnint128(
        make_address_balance_item_key(recipient_addr), kFirstTransferAmount,
        std::to_underlying(StorageDeltaFlags::None)));

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
        std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx2.request_nnint128(
        make_address_balance_item_key(recipient_addr), kSecondTransferAmount,
        std::to_underlying(StorageDeltaFlags::None)));

    ASSERT_TRUE(invoke_tx2.sync_balance_locks());
    invoke_tx2.commit();
    invoke_tx2.release_blocktx_reference();
  }

  // Commit the block transaction
  ASSERT_TRUE(block_tx.get().close(DbHash{}));
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

TEST_F(HogDBTest, DbHashDeltaAuthLockMatchesFullStateHash)
{
  Address addr = make_address(kAddress + 15);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    AuthDataChangeArgs args{};
    args.new_nonce_config =
        NonceConfig{.accepted_keys = 1, .required_weight = 0};
    ASSERT_TRUE(invoke_tx.request_auth_data(
        addr, args, std::to_underlying(StorageDeltaFlags::None)));
  });
}

TEST_F(HogDBTest, DbHashDeltaBalanceLockMatchesFullStateHash)
{
  Address addr = make_address(kAddress + 10);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000));

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_nnint128(make_address_balance_item_key(addr),
                                           123,
                                           std::to_underlying(
                                               StorageDeltaFlags::None)));
  });
}

TEST_F(HogDBTest, DbHashDeltaNNInt128LockMatchesFullStateHash)
{
  Address contract_addr = make_address(kAddress + 11);
  setup_contract_address(contract_addr);
  ItemKey key = make_item_key(contract_addr, 1);

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_nnint128(
        key, 321, std::to_underlying(StorageDeltaFlags::None)));
  });
}

TEST_F(HogDBTest, CommittedLocksPersistCanonicalXdrBytes)
{
  Address balance_addr = make_address(kAddress + 60);
  ASSERT_TRUE(db_.write_address_balance(balance_addr, 10'000));

  commit_block_and_return_db_hash(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_nnint128(
        make_address_balance_item_key(balance_addr), 123,
        std::to_underlying(StorageDeltaFlags::None)));
  });

  auto balance_key = make_address_key(balance_addr, Prefix::kBalance);
  auto raw_balance =
      db_.get_ramdb().get_if_type(std::as_bytes(std::span(balance_key)),
                                  kNNInt128);
  ASSERT_TRUE(raw_balance);
  EXPECT_TRUE(std::equal(raw_balance->begin(), raw_balance->end(),
                         int128_to_xdr_bytes(10'123).begin()));
  EXPECT_FALSE(std::equal(raw_balance->begin(), raw_balance->end(),
                          raw_int128_le_bytes(10'123).begin()));

  Address contract_addr = make_address(kAddress + 61);
  setup_contract_address(contract_addr);
  ItemKey key = make_item_key(contract_addr, 9);

  commit_block_and_return_db_hash(2, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_nnint128(
        key, 321, std::to_underlying(StorageDeltaFlags::None)));
  });

  auto raw_item = db_.read_contract_data_item(key);
  ASSERT_TRUE(raw_item);
  EXPECT_TRUE(std::equal(raw_item->value.begin(), raw_item->value.end(),
                         int128_to_xdr_bytes(321).begin()));
  EXPECT_FALSE(std::equal(raw_item->value.begin(), raw_item->value.end(),
                          raw_int128_le_bytes(321).begin()));
}

TEST_F(HogDBTest, DbHashDeltaNNIntLockUsesCanonicalXdrBytes)
{
  Address contract_addr = make_address(kAddress + 62);
  setup_contract_address(contract_addr);
  ItemKey key = make_item_key(contract_addr, 10);
  auto db_key = make_contract_data_key(key.as_bytes());
  auto balance_key = make_address_key(contract_addr, Prefix::kBalance);
  DbHash base_hash = db_.compute_full_db_hash();
  constexpr int128_t kInitialBalance = 10'000'000;

  PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                 1);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  ASSERT_TRUE(
      invoke_tx.request_nnint128(key, 321,
                                 std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  auto hash_result = block_tx.get().close(base_hash);
  ASSERT_TRUE(hash_result.has_value());
  DbHash incremental_hash = *hash_result;
  ASSERT_TRUE(block_tx.get().commit());

  auto final_balance = db_.read_address_balance(contract_addr);
  ASSERT_TRUE(final_balance);

  LtHash16 canonical = lthash16_from_db_hash(base_hash);
  auto initial_balance_bytes = int128_to_xdr_bytes(kInitialBalance);
  lthash_remove_item(canonical, std::as_bytes(std::span(balance_key)),
                     kNNInt128, initial_balance_bytes);
  lthash_add_item(canonical, std::as_bytes(std::span(balance_key)), kNNInt128,
                  int128_to_xdr_bytes(*final_balance));
  auto canonical_bytes = int128_to_xdr_bytes(321);
  lthash_add_item(canonical, db_key, kNNInt128, canonical_bytes);
  DbHash canonical_hash = db_hash_from_lthash16(canonical);

  LtHash16 raw = lthash16_from_db_hash(base_hash);
  lthash_remove_item(raw, std::as_bytes(std::span(balance_key)), kNNInt128,
                     initial_balance_bytes);
  lthash_add_item(raw, std::as_bytes(std::span(balance_key)), kNNInt128,
                  raw_int128_le_bytes(*final_balance));
  auto raw_bytes = raw_int128_le_bytes(321);
  lthash_add_item(raw, db_key, kNNInt128, raw_bytes);
  DbHash raw_hash = db_hash_from_lthash16(raw);

  EXPECT_EQ(incremental_hash, canonical_hash);
  EXPECT_NE(incremental_hash, raw_hash);
  EXPECT_EQ(incremental_hash, db_.compute_full_db_hash());
}

TEST_F(HogDBTest, InvariantsAcceptCanonicalBalanceBytes)
{
  Address contract_addr = make_address(kAddress + 63);
  setup_contract_address(contract_addr);
  ItemKey key = make_item_key(contract_addr, 11);
  DbHash base_hash = db_.compute_full_db_hash();

  db_.enable_debug_invariants(cost_model_);

  PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                 1);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  ASSERT_TRUE(
      invoke_tx.request_nnint128(key, 321,
                                 std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  auto hash_result = block_tx.get().close(base_hash);
  ASSERT_TRUE(hash_result.has_value());
  ASSERT_TRUE(block_tx.get().commit());

  EXPECT_TRUE(db_.check_invariants());
}

TEST_F(HogDBTest, DbHashDeltaBytestringLockMatchesFullStateHash)
{
  Address contract_addr = make_address(kAddress + 12);
  setup_contract_address(contract_addr);
  ItemKey key = make_item_key(contract_addr, 2);

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    constexpr std::array<std::byte, 4> kValue = {std::byte{0x01},
                                                 std::byte{0x02},
                                                 std::byte{0x03},
                                                 std::byte{0x04}};
    ASSERT_TRUE(
        invoke_tx.request_bytes(key, std::span(kValue),
                                std::to_underlying(StorageDeltaFlags::None)));
  });
}

TEST_F(HogDBTest, DbHashDeltaDeletionLockMatchesFullStateHash)
{
  Address contract_addr = make_address(kAddress + 13);
  setup_contract_address(contract_addr);
  ItemKey key = make_item_key(contract_addr, 3);

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    constexpr std::array<std::byte, 2> kValue = {std::byte{0xAA},
                                                 std::byte{0xBB}};
    ASSERT_TRUE(
        invoke_tx.request_bytes(key, std::span(kValue),
                                std::to_underlying(StorageDeltaFlags::None)));
  });

  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_deletion(key, std::to_underlying(
                                                    StorageDeltaFlags::None)));
  });
}

TEST_F(HogDBTest, DbHashDeltaNewContractLockMatchesFullStateHash)
{
  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(1);

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_new_contract(contract));
  });
}

TEST_F(HogDBTest, DbHashDeltaNewDeploymentLockMatchesFullStateHash)
{
  Address addr = make_address(kAddress + 14);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_deploy_contract(
        addr,
        DeployedContractHash{.hash = sha256(std::as_bytes(std::span("deploy"))),
                             .contract_size = 15}));
  });
}

TEST_F(HogDBTest, DbHashDeltaReplayCacheLockMatchesFullStateHash)
{
  Address addr = make_address(kAddress + 16);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(5, 42), std::to_underlying(StorageDeltaFlags::None)));
  });
}

TEST_F(HogDBTest, DbHashDeltaRetrocausalQueueLockMatchesFullStateHash)
{
  Address contract_addr = make_address(kAddress + 17);
  setup_contract_address(contract_addr);
  ItemKey rcq_key = make_item_key(contract_addr, 4);

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x11});
    delta.insert.added_entries.push_back(QueueEntry{0x22});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// Test that RCQ block contents are integrity-protected by the db hash.
//
// The test:
// 1. Creates an RCQ with entries that span multiple blocks
// 2. Commits and captures the db hash
// 3. Directly modifies RCQ block data in ramdb (simulating an attacker)
// 4. Recomputes the db hash
// 5. Asserts that the db hash changed
//
// Expected behavior: Root hash SHOULD change (integrity protected)
TEST_F(HogDBTest, RCQBlockIntegrityBlockContentModificationChangesDbHash)
{
  constexpr size_t kEntriesPerBlock = 100;
  constexpr size_t kNumBlocks = 3;

  // Setup: create contract with enough storage tokens
  Address contract_addr = make_address(kAddress);
  ItemKey rcq_key = make_key(kAddress, 1);

  ASSERT_TRUE(db_.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db_.write_address_metadata(
      contract_addr,
      DeployedContractHash{.hash = sha256(std::as_bytes(std::span("x"))),
                           .contract_size = 10}));

  // Create RCQ with entries across multiple blocks
  // Each block gets kEntriesPerBlock entries
  for (size_t block_idx = 0; block_idx < kNumBlocks; block_idx++) {
    BlockHeight height = block_idx + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);

    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    // Create delta with entries for this block
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.resize(kEntriesPerBlock);
    for (size_t i = 0; i < kEntriesPerBlock; i++) {
      // Each entry contains a single byte: block_idx
      delta.insert.added_entries[i] =
          QueueEntry{static_cast<uint8_t>(block_idx)};
    }

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));

    // sync_balance_locks() handles storage cost deduction
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();

    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Capture the db hash after normal operations
  // The db hash has been updated by the BlockTX commits above
  DbHash original_db_hash = db_.compute_full_db_hash();

  // Read block 1's data
  uint64_t target_block_number = 1;
  auto block_result =
      db_.read_retrocausal_queue_block(rcq_key, target_block_number);
  ASSERT_TRUE(block_result);
  RetrocausalQueueBlock original_block = *block_result;

  ASSERT_GT(original_block.entries.size(), 0);

  // Now directly modify block 1's data in ramdb (simulating an attacker)
  // This bypasses the transaction layer and db hash updates
  RetrocausalQueueBlock modified_block = original_block;
  modified_block.entries[0] =
      QueueEntry{255}; // Change first entry from 0 to 255

  // Write the modified block directly to ramdb, bypassing the db hash
  ASSERT_TRUE(db_.write_retrocausal_queue_block(rcq_key, modified_block));
  ASSERT_TRUE(db_.commit());

  // Verify the block content was actually modified
  auto modified_block_result =
      db_.read_retrocausal_queue_block(rcq_key, target_block_number);
  ASSERT_TRUE(modified_block_result);
  auto reread_block = *modified_block_result;
  EXPECT_NE(original_block.entries[0][0], reread_block.entries[0][0]);
  EXPECT_EQ(255, reread_block.entries[0][0]);

  // Compute hash of block content to prove it changed
  auto original_block_serialized = x23::serialize(original_block);
  auto modified_block_serialized = x23::serialize(reread_block);
  SHA256Hash original_block_hash =
      sha256(std::as_bytes(std::span(original_block_serialized)));
  SHA256Hash modified_block_hash =
      sha256(std::as_bytes(std::span(modified_block_serialized)));
  ASSERT_NE(original_block_hash, modified_block_hash);

  // Recompute the db hash from ramdb to detect the inconsistency
  // If blocks are properly integrity-protected in the db hash, reloading from
  // the tampered ramdb will produce a different db hash
  DbHash new_db_hash = db_.compute_full_db_hash();

  // Blocks are included directly in the db hash, so tampering with block
  // contents in ramdb causes the reloaded db hash to differ from the
  // original.
  EXPECT_NE(original_db_hash, new_db_hash)
      << "Modifying RCQ block contents did NOT change the db hash!";
}

// Test that consuming blocks removes them from the db hash
TEST_F(HogDBTest, RCQBlockIntegrityConsumeRemovesBlocksFromDbHash)
{
  // Setup: create contract with enough storage tokens
  Address contract_addr = make_address(kAddress);
  ItemKey rcq_key = make_key(kAddress, 2);

  ASSERT_TRUE(db_.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db_.write_address_metadata(
      contract_addr,
      DeployedContractHash{.hash = sha256(std::as_bytes(std::span("consume"))),
                           .contract_size = 10}));

  constexpr size_t kEntriesPerBlock = 100;
  constexpr size_t kNumBlocks = 3;

  // Create blocks
  for (size_t block_idx = 0; block_idx < kNumBlocks; block_idx++) {
    BlockHeight height = block_idx + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.insert.added_entries.resize(kEntriesPerBlock);
    for (size_t i = 0; i < kEntriesPerBlock; i++) {
      delta.insert.added_entries[i] =
          QueueEntry{static_cast<uint8_t>(block_idx)};
    }

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  DbHash db_hash_before_consume = db_.compute_full_db_hash();

  // Consume the first block completely
  DbHash db_hash_after_consume;
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number = 1;
    delta.consume->past_block_consume.partial_consumed_number = 0;

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    {
      auto r = block_tx.get().close(db_hash_before_consume);
      ASSERT_TRUE(r.has_value());
      db_hash_after_consume = *r;
    }
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Verify incremental db hash produced by the block tx matches the db hash
  // generated from scratch
  DbHash full_db_hash = db_.compute_full_db_hash();
  EXPECT_EQ(db_hash_after_consume, full_db_hash);

  // Verify db hash changed after consuming block 1
  EXPECT_NE(db_hash_before_consume, db_hash_after_consume)
      << "db hash did not change after consuming block - block may not have "
         "been removed from hash";

  // Verify block 1 is not in ramdb anymore
  auto block_result = db_.read_retrocausal_queue_block(rcq_key, 1);
  EXPECT_FALSE(block_result.has_value())
      << "Block 1 should be removed from ramdb after consume";
}

// Test that deleting an RCQ removes all blocks from the db hash
TEST_F(HogDBTest, RCQBlockIntegrityDeleteRemovesAllBlocksFromDbHash)
{
  // Setup: create contract with enough storage tokens
  Address contract_addr = make_address(kAddress);
  ItemKey rcq_key = make_key(kAddress, 3);

  ASSERT_TRUE(db_.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db_.write_address_metadata(
      contract_addr,
      DeployedContractHash{.hash = sha256(std::as_bytes(std::span("delete"))),
                           .contract_size = 10}));

  constexpr size_t kEntriesPerBlock = 100;
  constexpr size_t kNumBlocks = 3;

  // Create blocks
  for (size_t block_idx = 0; block_idx < kNumBlocks; block_idx++) {
    BlockHeight height = block_idx + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.insert.added_entries.resize(kEntriesPerBlock);
    for (size_t i = 0; i < kEntriesPerBlock; i++) {
      delta.insert.added_entries[i] =
          QueueEntry{static_cast<uint8_t>(block_idx)};
    }

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  DbHash db_hash_before_delete = db_.compute_full_db_hash();

  // Delete the entire RCQ by consuming all entries
  DbHash db_hash_after_delete;
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    // Consume all blocks
    RetrocausalQueueDelta delta;
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number =
        kNumBlocks;
    delta.consume->past_block_consume.partial_consumed_number = 0;

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    {
      auto r = block_tx.get().close(db_hash_before_delete);
      ASSERT_TRUE(r.has_value());
      db_hash_after_delete = *r;
    }
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Verify incremental db hash produced by the block tx matches the db hash
  // generated from scratch
  DbHash full_db_hash = db_.compute_full_db_hash();
  EXPECT_EQ(db_hash_after_delete, full_db_hash);

  // Verify db hash changed after deleting all blocks
  EXPECT_NE(db_hash_before_delete, db_hash_after_delete)
      << "db hash did not change after deleting RCQ - blocks may not have been "
         "removed from hash";

  // Verify all blocks are removed from ramdb
  for (uint64_t block_num = 1; block_num <= kNumBlocks; block_num++) {
    auto block_result = db_.read_retrocausal_queue_block(rcq_key, block_num);
    EXPECT_FALSE(block_result.has_value())
        << "Block " << block_num
        << " should be removed from ramdb after delete";
  }
}

TEST_F(HogDBTest, RCQBlockIntegrityExplicitDeletionRemovesAllBlocksFromDbHash)
{
  // Test that explicit deletion via request_deletion (DeletionLock path)
  // properly removes all RCQ blocks from both DAO and db hash.

  // Setup: create contract with enough storage tokens
  Address contract_addr = make_address(kAddress);
  ItemKey rcq_key = make_key(kAddress, 4);

  ASSERT_TRUE(db_.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db_.write_address_metadata(
      contract_addr,
      DeployedContractHash{.hash = sha256(std::as_bytes(std::span("delete"))),
                           .contract_size = 10}));

  constexpr size_t kEntriesPerBlock = 100;
  constexpr size_t kNumBlocks = 3;

  // Create blocks
  for (size_t block_idx = 0; block_idx < kNumBlocks; block_idx++) {
    BlockHeight height = block_idx + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.insert.added_entries.resize(kEntriesPerBlock);
    for (size_t i = 0; i < kEntriesPerBlock; i++) {
      delta.insert.added_entries[i] =
          QueueEntry{static_cast<uint8_t>(block_idx)};
    }

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  DbHash db_hash_before_delete = db_.compute_full_db_hash();

  // Explicitly delete the RCQ using request_deletion (goes through
  // DeletionLock)
  DbHash db_hash_after_delete;
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_deletion(
        rcq_key, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    {
      auto r = block_tx.get().close(db_hash_before_delete);
      ASSERT_TRUE(r.has_value());
      db_hash_after_delete = *r;
    }
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Verify incremental db hash produced by the block tx matches the db hash
  // generated from scratch
  DbHash full_db_hash = db_.compute_full_db_hash();
  EXPECT_EQ(db_hash_after_delete, full_db_hash);

  // Verify db hash changed after deleting all blocks
  EXPECT_NE(db_hash_before_delete, db_hash_after_delete)
      << "db hash did not change after explicit deletion - blocks may not have "
         "been removed from db";

  // Verify all blocks are removed from ramdb
  for (uint64_t block_num = 1; block_num <= kNumBlocks; block_num++) {
    auto block_result = db_.read_retrocausal_queue_block(rcq_key, block_num);
    EXPECT_FALSE(block_result.has_value())
        << "Block " << block_num
        << " should be removed from ramdb after explicit deletion";
  }

  // Verify metadata is also removed
  auto meta_result = db_.read_retrocausal_queue_metadata(rcq_key);
  EXPECT_FALSE(meta_result.has_value())
      << "Metadata should be removed from ramdb after explicit deletion";
}

TEST_F(HogDBTest,
       RCQBlockIntegrityTypeChangeBytestringRemovesAllBlocksFromDbHash)
{
  // RCQ type-change cleanup handled by ItemLock::stage_committed_changes.
  // Test that type change from RCQ to Bytestring properly removes all RCQ
  // blocks from both DAO and db hash.

  // Setup: create contract with enough storage tokens
  Address contract_addr = make_address(kAddress);
  ItemKey rcq_key = make_key(kAddress, 4);

  ASSERT_TRUE(db_.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db_.write_address_metadata(
      contract_addr,
      DeployedContractHash{.hash = sha256(std::as_bytes(std::span("delete"))),
                           .contract_size = 10}));

  constexpr size_t kEntriesPerBlock = 100;
  constexpr size_t kNumBlocks = 3;

  // Create blocks
  for (size_t block_idx = 0; block_idx < kNumBlocks; block_idx++) {
    BlockHeight height = block_idx + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.insert.added_entries.resize(kEntriesPerBlock);
    for (size_t i = 0; i < kEntriesPerBlock; i++) {
      delta.insert.added_entries[i] =
          QueueEntry{static_cast<uint8_t>(block_idx)};
    }

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  DbHash db_hash_before_type_change = db_.compute_full_db_hash();

  // Explicitly delete the RCQ using request_deletion (goes through
  // DeletionLock)
  DbHash db_hash_after_type_change;
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_bytes(
        rcq_key, std::span<const std::byte>(std::as_bytes(std::span("test"))),
        std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    {
      auto r = block_tx.get().close(db_hash_before_type_change);
      ASSERT_TRUE(r.has_value());
      db_hash_after_type_change = *r;
    }
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Verify incremental db hash produced by the block tx matches the db hash
  // generated from scratch
  DbHash full_db_hash = db_.compute_full_db_hash();
  EXPECT_EQ(db_hash_after_type_change, full_db_hash);

  // Verify db hash changed after deleting all blocks
  EXPECT_NE(db_hash_before_type_change, db_hash_after_type_change)
      << "db hash did not change after type change - blocks may not have "
         "been removed from db";

  // Verify all blocks are removed from ramdb
  for (uint64_t block_num = 1; block_num <= kNumBlocks; block_num++) {
    auto block_result = db_.read_retrocausal_queue_block(rcq_key, block_num);
    EXPECT_FALSE(block_result.has_value())
        << "Block " << block_num
        << " should be removed from ramdb after explicit deletion";
  }

  // Verify metadata is also removed
  auto meta_result = db_.read_retrocausal_queue_metadata(rcq_key);
  EXPECT_FALSE(meta_result.has_value())
      << "Metadata should be removed from ramdb after explicit deletion";
}

TEST_F(HogDBTest, RCQBlockIntegrityTypeChangeNNIntRemovesAllBlocksFromDbHash)
{
  // RCQ type-change cleanup handled by ItemLock::stage_committed_changes.
  // Test that type change from RCQ to NNInt properly removes all RCQ blocks
  // from both DAO and db hash.

  // Setup: create contract with enough storage tokens
  Address contract_addr = make_address(kAddress);
  ItemKey rcq_key = make_key(kAddress, 4);

  ASSERT_TRUE(db_.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db_.write_address_metadata(
      contract_addr,
      DeployedContractHash{.hash = sha256(std::as_bytes(std::span("delete"))),
                           .contract_size = 10}));

  constexpr size_t kEntriesPerBlock = 100;
  constexpr size_t kNumBlocks = 3;

  // Create blocks
  for (size_t block_idx = 0; block_idx < kNumBlocks; block_idx++) {
    BlockHeight height = block_idx + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.insert.added_entries.resize(kEntriesPerBlock);
    for (size_t i = 0; i < kEntriesPerBlock; i++) {
      delta.insert.added_entries[i] =
          QueueEntry{static_cast<uint8_t>(block_idx)};
    }

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  DbHash db_hash_before_type_change = db_.compute_full_db_hash();

  // Explicitly delete the RCQ using request_deletion (goes through
  // DeletionLock)
  DbHash db_hash_after_type_change;
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_nnint128(
        rcq_key, 100, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    {
      auto r = block_tx.get().close(db_hash_before_type_change);
      ASSERT_TRUE(r.has_value());
      db_hash_after_type_change = *r;
    }
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Verify incremental db hash produced by the block tx matches the db hash
  // generated from scratch
  DbHash full_db_hash = db_.compute_full_db_hash();
  EXPECT_EQ(db_hash_after_type_change, full_db_hash);

  // Verify db hash changed after deleting all blocks
  EXPECT_NE(db_hash_before_type_change, db_hash_after_type_change)
      << "db hash did not change after type change - blocks may not have "
         "been removed from db";

  // Verify all blocks are removed from ramdb
  for (uint64_t block_num = 1; block_num <= kNumBlocks; block_num++) {
    auto block_result = db_.read_retrocausal_queue_block(rcq_key, block_num);
    EXPECT_FALSE(block_result.has_value())
        << "Block " << block_num
        << " should be removed from ramdb after explicit deletion";
  }

  // Verify metadata is also removed
  auto meta_result = db_.read_retrocausal_queue_metadata(rcq_key);
  EXPECT_FALSE(meta_result.has_value())
      << "Metadata should be removed from ramdb after explicit deletion";
}

// Test that a fully consumed RCQ leaves no residual state in the db hash.
// Two databases with identical base state should produce the same hash when
// one has had an RCQ inserted and fully consumed over several blocks.
TEST_F(HogDBTest, RCQFullyConsumedDbHashMatchesEmptyDb)
{
  Address contract_addr = make_address(kAddress);
  ItemKey rcq_key = make_key(kAddress, 5);

  // Setup contract on db_
  ASSERT_TRUE(db_.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db_.write_address_metadata(
      contract_addr, DeployedContractHash{.hash = sha256(std::as_bytes(
                                              std::span("rcq_consumed"))),
                                          .contract_size = 10}));

  // Create second database with identical base state
  ErrnoStatus<Directory> dir2 = Directory::open_temporary();
  ASSERT_TRUE(static_cast<bool>(dir2));
  constexpr size_t kHeapSize = 10_MiB;
  auto raw_db2 = Database::open(dir2->get_path(),
                                Database::estimate_heap_size(kHeapSize), 0);
  ASSERT_TRUE(raw_db2);
  HogDBDAO db2(RAMDB(std::move(*raw_db2)));

  ASSERT_TRUE(db2.write_address_balance(contract_addr, 10'000'000));
  ASSERT_TRUE(db2.write_address_metadata(
      contract_addr, DeployedContractHash{.hash = sha256(std::as_bytes(
                                              std::span("rcq_consumed"))),
                                          .contract_size = 10}));

  // Insert RCQ entries on db_ over several blocks
  constexpr size_t kNumBlocks = 3;
  for (size_t block_idx = 0; block_idx < kNumBlocks; block_idx++) {
    BlockHeight height = block_idx + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(
        QueueEntry{static_cast<uint8_t>(block_idx)});
    delta.insert.added_entries.push_back(
        QueueEntry{static_cast<uint8_t>(block_idx + 0x10)});

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Fully consume all entries
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number =
        kNumBlocks;
    delta.consume->past_block_consume.partial_consumed_number = 0;

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Sanity: all blocks and metadata should be gone
  for (uint64_t block_num = 1; block_num <= kNumBlocks; block_num++) {
    auto block_result = db_.read_retrocausal_queue_block(rcq_key, block_num);
    EXPECT_FALSE(block_result.has_value())
        << "Block " << block_num << " should be removed after full consumption";
  }
  auto meta_result = db_.read_retrocausal_queue_metadata(rcq_key);
  EXPECT_FALSE(meta_result.has_value())
      << "Metadata item should be removed after full consumption";

  // The fully consumed db should hash-match the empty db
  DbHash consumed_hash = db_.compute_full_db_hash();
  DbHash empty_hash = db2.compute_full_db_hash();
  EXPECT_EQ(consumed_hash, empty_hash)
      << "Fully consumed RCQ should leave no residual state in the db hash";
}

// Replay cache: GC on next modification updates hash correctly.
// Height-3 nonces are GC'd when block height exceeds their expiry.
TEST_F(HogDBTest, ReplayCacheGCOnNextModificationUpdatesHash)
{
  Address addr = make_address(kAddress + 20);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  // Block 1: insert nonce with expiry height 3
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(3, 100), std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: insert another nonce also with expiry height 3
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(3, 200), std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 5: insert nonce with expiry height 7 — triggers GC of height-3
  // entries since block_height(5) > expiry(3)
  commit_block_expect_db_hash_matches_full(5, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(7, 300), std::to_underlying(StorageDeltaFlags::None)));
  });
}

TEST_F(HogDBTest, ReplayCacheGCOnlyRunsOnCommittedReplayCacheTouch)
{
  Address addr = make_address(kAddress + 52);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  auto populate_two_expiring_entries = [&] {
    commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
      ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 100),
          std::to_underlying(StorageDeltaFlags::None)));
    });
    commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
      ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 200),
          std::to_underlying(StorageDeltaFlags::None)));
    });
  };

  populate_two_expiring_entries();

  {
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   5);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(7, 300), std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());

    invoke_tx.abort();
    invoke_tx.release_blocktx_reference();

    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  auto replay_meta_after_abort = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(replay_meta_after_abort);
  ASSERT_EQ(2, replay_meta_after_abort->size);
  ASSERT_EQ(1, replay_meta_after_abort->heights.size());
  EXPECT_EQ(3, replay_meta_after_abort->heights[0].height);
  EXPECT_EQ(2, replay_meta_after_abort->heights[0].nonce_count);

  auto replay_data_after_abort = db_.read_address_replay_cache_data(addr, 3);
  ASSERT_TRUE(replay_data_after_abort);
  ASSERT_EQ(2, replay_data_after_abort->nonces.size());
  EXPECT_EQ(100, replay_data_after_abort->nonces[0]);
  EXPECT_EQ(200, replay_data_after_abort->nonces[1]);

  commit_block_expect_db_hash_matches_full(6, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(8, 300), std::to_underlying(StorageDeltaFlags::None)));
  });

  auto replay_meta_after_commit = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(replay_meta_after_commit);
  ASSERT_EQ(1, replay_meta_after_commit->size);
  ASSERT_EQ(1, replay_meta_after_commit->heights.size());
  EXPECT_EQ(8, replay_meta_after_commit->heights[0].height);
  EXPECT_EQ(1, replay_meta_after_commit->heights[0].nonce_count);

  auto replay_data_after_gc = db_.read_address_replay_cache_data(addr, 3);
  EXPECT_FALSE(replay_data_after_gc.has_value());
}

TEST_F(HogDBTest, ReplayCacheGCRefundsCommittedNetStorageDecrease)
{
  auto populate_two_expiring_entries = [&](Address addr) {
    commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
      ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 100),
          std::to_underlying(StorageDeltaFlags::None)));
    });
    commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
      ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 200),
          std::to_underlying(StorageDeltaFlags::None)));
    });
  };

  Address conflict_addr = make_address(kAddress + 50);
  ASSERT_TRUE(db_.write_address_balance(conflict_addr,
                                        cost_model_.get_replay_cache_cost(2)));
  populate_two_expiring_entries(conflict_addr);

  auto conflict_balance = db_.read_address_balance(conflict_addr);
  ASSERT_TRUE(conflict_balance);
  EXPECT_EQ(0, *conflict_balance);

  {
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   5);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);
    invoke_tx.create_savepoint();

    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        conflict_addr, Uniquifier(7, 300),
        std::to_underlying(StorageDeltaFlags::None)));

    auto sync_result = invoke_tx.sync_balance_locks();
    ASSERT_FALSE(sync_result.has_value());
    EXPECT_EQ(HogDBCode::kConflict, sync_result.error().code());

    invoke_tx.rollback_to_savepoint(0);
    invoke_tx.release_blocktx_reference();
  }

  Address charged_addr = make_address(kAddress + 51);
  ASSERT_TRUE(
      db_.write_address_balance(charged_addr,
                                cost_model_.get_replay_cache_cost(2) +
                                    cost_model_.kReplayCacheCostPerEntry));
  populate_two_expiring_entries(charged_addr);

  auto charged_balance_before_gc = db_.read_address_balance(charged_addr);
  ASSERT_TRUE(charged_balance_before_gc);
  EXPECT_EQ(cost_model_.kReplayCacheCostPerEntry, *charged_balance_before_gc);

  commit_block_expect_db_hash_matches_full(5, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        charged_addr, Uniquifier(7, 300),
        std::to_underlying(StorageDeltaFlags::None)));
  });

  auto charged_balance_after_gc = db_.read_address_balance(charged_addr);
  ASSERT_TRUE(charged_balance_after_gc);
  EXPECT_EQ(cost_model_.get_replay_cache_cost(2) +
                cost_model_.kReplayCacheCostPerEntry -
                cost_model_.get_replay_cache_cost(1),
            *charged_balance_after_gc);

  auto replay_meta = db_.read_address_replay_cache_meta(charged_addr);
  ASSERT_TRUE(replay_meta);
  ASSERT_EQ(1, replay_meta->size);
  ASSERT_EQ(1, replay_meta->heights.size());
  EXPECT_EQ(7, replay_meta->heights[0].height);
  EXPECT_EQ(1, replay_meta->heights[0].nonce_count);
}

TEST_F(HogDBTest, ReplayCacheGCPrunesMultipleHeightsAtOnce)
{
  // Seed three distinct expiry heights (3, 5, 8) so the GC at block 7 has to
  // walk past more than one ReplayCacheHeightCount entry: heights 3 and 5
  // expire; height 8 survives.
  //
  // Any intermediate block that touches the replay cache would trigger GC of
  // already-expired heights, so we seed at ascending block numbers
  // (1, 2, 3) where none of the seeded heights are yet expired relative to
  // the seeding block height. This preserves meta == {3:1, 5:1, 8:1} up to
  // the GC trigger at block 7.
  Address addr = make_address(kAddress + 53);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(3, 100), std::to_underlying(StorageDeltaFlags::None)));
  });
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(5, 200), std::to_underlying(StorageDeltaFlags::None)));
  });
  commit_block_expect_db_hash_matches_full(3, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(8, 800), std::to_underlying(StorageDeltaFlags::None)));
  });

  auto replay_meta_before_gc = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(replay_meta_before_gc);
  ASSERT_EQ(3, replay_meta_before_gc->size);
  ASSERT_EQ(3, replay_meta_before_gc->heights.size());
  EXPECT_EQ(3, replay_meta_before_gc->heights[0].height);
  EXPECT_EQ(1, replay_meta_before_gc->heights[0].nonce_count);
  EXPECT_EQ(5, replay_meta_before_gc->heights[1].height);
  EXPECT_EQ(1, replay_meta_before_gc->heights[1].nonce_count);
  EXPECT_EQ(8, replay_meta_before_gc->heights[2].height);
  EXPECT_EQ(1, replay_meta_before_gc->heights[2].nonce_count);

  auto balance_before_gc = db_.read_address_balance(addr);
  ASSERT_TRUE(balance_before_gc);

  // Block 7 adds a single entry at expiry 7. GC must prune heights 3 and 5
  // (2 entries) but keep height 8 (1 entry); the new addition lives at
  // height 7. Net size goes from 3 to 2, so the balance refund equals
  // cost(3) - cost(2) = kReplayCacheCostPerEntry.
  commit_block_expect_db_hash_matches_full(7, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(7, 700), std::to_underlying(StorageDeltaFlags::None)));
  });

  auto replay_meta_after_gc = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(replay_meta_after_gc);
  ASSERT_EQ(2, replay_meta_after_gc->size);
  ASSERT_EQ(2, replay_meta_after_gc->heights.size());
  EXPECT_EQ(7, replay_meta_after_gc->heights[0].height);
  EXPECT_EQ(1, replay_meta_after_gc->heights[0].nonce_count);
  EXPECT_EQ(8, replay_meta_after_gc->heights[1].height);
  EXPECT_EQ(1, replay_meta_after_gc->heights[1].nonce_count);

  // Pruned heights' data rows are gone; the surviving height-8 data is
  // unchanged.
  EXPECT_FALSE(db_.read_address_replay_cache_data(addr, 3).has_value());
  EXPECT_FALSE(db_.read_address_replay_cache_data(addr, 5).has_value());

  auto height_8_data = db_.read_address_replay_cache_data(addr, 8);
  ASSERT_TRUE(height_8_data);
  ASSERT_EQ(1, height_8_data->nonces.size());
  EXPECT_EQ(800, height_8_data->nonces[0]);

  auto height_7_data = db_.read_address_replay_cache_data(addr, 7);
  ASSERT_TRUE(height_7_data);
  ASSERT_EQ(1, height_7_data->nonces.size());
  EXPECT_EQ(700, height_7_data->nonces[0]);

  auto balance_after_gc = db_.read_address_balance(addr);
  ASSERT_TRUE(balance_after_gc);
  EXPECT_EQ(cost_model_.get_replay_cache_cost(3) -
                cost_model_.get_replay_cache_cost(2),
            *balance_after_gc - *balance_before_gc);
}

TEST_F(HogDBTest, ReplayCacheNonceLifecycleAddThenGCAtExpiryBoundary)
{
  // End-to-end lifecycle for a single nonce:
  //   * added at block 1 with expiry 5,
  //   * still present after a replay-cache touch at block 5 (height == expiry
  //     is NOT pruned; GC uses strict less-than),
  //   * pruned by the first replay-cache touch at block 6 (height < 6).
  Address addr = make_address(kAddress + 54);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(5, 100), std::to_underlying(StorageDeltaFlags::None)));
  });

  auto meta_after_add = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(meta_after_add);
  ASSERT_EQ(1, meta_after_add->size);
  ASSERT_EQ(1, meta_after_add->heights.size());
  EXPECT_EQ(5, meta_after_add->heights[0].height);
  EXPECT_EQ(1, meta_after_add->heights[0].nonce_count);

  auto data_after_add = db_.read_address_replay_cache_data(addr, 5);
  ASSERT_TRUE(data_after_add);
  ASSERT_EQ(1, data_after_add->nonces.size());
  EXPECT_EQ(100, data_after_add->nonces[0]);

  // Block 5: replay-cache touch at the exact expiry height must not GC the
  // entry. The new addition (expiry 8) lives alongside the original.
  commit_block_expect_db_hash_matches_full(5, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(8, 200), std::to_underlying(StorageDeltaFlags::None)));
  });

  auto meta_at_expiry = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(meta_at_expiry);
  ASSERT_EQ(2, meta_at_expiry->size);
  ASSERT_EQ(2, meta_at_expiry->heights.size());
  EXPECT_EQ(5, meta_at_expiry->heights[0].height);
  EXPECT_EQ(1, meta_at_expiry->heights[0].nonce_count);
  EXPECT_EQ(8, meta_at_expiry->heights[1].height);
  EXPECT_EQ(1, meta_at_expiry->heights[1].nonce_count);

  auto data_at_expiry = db_.read_address_replay_cache_data(addr, 5);
  ASSERT_TRUE(data_at_expiry);
  ASSERT_EQ(1, data_at_expiry->nonces.size());
  EXPECT_EQ(100, data_at_expiry->nonces[0]);

  // Block 6: first block where the expiry-5 entry is strictly past its
  // expiry. The replay-cache touch triggers GC of the original nonce.
  commit_block_expect_db_hash_matches_full(6, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(9, 300), std::to_underlying(StorageDeltaFlags::None)));
  });

  auto meta_after_gc = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(meta_after_gc);
  ASSERT_EQ(2, meta_after_gc->size);
  ASSERT_EQ(2, meta_after_gc->heights.size());
  EXPECT_EQ(8, meta_after_gc->heights[0].height);
  EXPECT_EQ(1, meta_after_gc->heights[0].nonce_count);
  EXPECT_EQ(9, meta_after_gc->heights[1].height);
  EXPECT_EQ(1, meta_after_gc->heights[1].nonce_count);

  EXPECT_FALSE(db_.read_address_replay_cache_data(addr, 5).has_value());
}

// Auth data: create then delete matches empty db.
// Creating and then deleting auth data should leave no residual state.
TEST_F(HogDBTest, AuthDataCreateThenDeleteMatchesEmptyDb)
{
  Address addr = make_address(kAddress + 23);

  // Setup identical base state on both databases
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  ErrnoStatus<Directory> dir2 = Directory::open_temporary();
  ASSERT_TRUE(static_cast<bool>(dir2));
  constexpr size_t kHeapSize = 10_MiB;
  auto raw_db2 = Database::open(dir2->get_path(),
                                Database::estimate_heap_size(kHeapSize), 0);
  ASSERT_TRUE(raw_db2);
  HogDBDAO db2(RAMDB(std::move(*raw_db2)));

  ASSERT_TRUE(db2.write_address_balance(addr, 10'000'000));

  // Block 1 on db_: create auth data with nonce config
  {
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   1);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    AuthDataChangeArgs args{};
    args.new_nonce_config =
        NonceConfig{.accepted_keys = 1, .required_weight = 0};
    ASSERT_TRUE(invoke_tx.request_auth_data(
        addr, args, std::to_underlying(StorageDeltaFlags::None)));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Block 2 on db_: delete the auth data
  {
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   2);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_deletion(
        make_address_auth_data_item_key(addr),
        std::to_underlying(StorageDeltaFlags::None)));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.commit();
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  // db2 has no auth operations — hashes should match
  DbHash created_then_deleted_hash = db_.compute_full_db_hash();
  DbHash empty_hash = db2.compute_full_db_hash();
  EXPECT_EQ(created_then_deleted_hash, empty_hash)
      << "Auth data create-then-delete should leave no residual state";
}

// Auth data: create then modify hash correctness.
// Incremental hash matches full hash after create and then modify.
TEST_F(HogDBTest, AuthDataCreateThenModifyHashCorrectness)
{
  Address addr = make_address(kAddress + 24);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  // Block 1: create auth data with nonce config
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    AuthDataChangeArgs args{};
    args.new_nonce_config =
        NonceConfig{.accepted_keys = 1, .required_weight = 0};
    ASSERT_TRUE(invoke_tx.request_auth_data(
        addr, args, std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: modify auth data by adding a public key signer
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    AuthDataChangeArgs args{};
    PublicKey key;
    key.ed25519().fill(0x42);
    args.auth_data_change_signers.push_back(AuthDataChangeSigner{
        .public_key_id = 0,
        .public_key = x23::Ptr<PublicKeyConfig>(
            new PublicKeyConfig{.key = key, .max_weight = 100})});
    ASSERT_TRUE(invoke_tx.request_auth_data(
        addr, args, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// BugHunt: Delete a bytestring then create an NNInt128 at the same key
// in a single InvokeTX. The exclusive lock type change must be correctly
// reflected in the incremental hash.
TEST_F(HogDBTest, TypeChangeByteToNNIntSameInvokeTX)
{
  Address addr = make_address(kAddress + 30);
  ItemKey data_key = make_key(kAddress + 30, 1);
  setup_contract_address(addr);

  // Block 1: write a bytestring
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    std::string data("some bytestring data");
    ASSERT_TRUE(
        invoke_tx.request_bytes(data_key, std::as_bytes(std::span(data)),
                                std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: delete the bytestring and create an NNInt128 at the same key
  // This triggers an exclusive lock due to type change within one InvokeTX
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_deletion(
        data_key, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.request_nnint128(
        data_key, 0, 42, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// BugHunt: Delete a bytestring then create an RCQ at the same key within one
// InvokeTX. Tests exclusive lock type change to RCQ which has complex
// multi-key hashing (metadata + blocks).
TEST_F(HogDBTest, TypeChangeBytesToRCQSameInvokeTX)
{
  Address addr = make_address(kAddress + 32);
  ItemKey data_key = make_key(kAddress + 32, 1);
  setup_contract_address(addr);

  // Block 1: write a bytestring
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    std::string data("bytestring to replace with RCQ");
    ASSERT_TRUE(
        invoke_tx.request_bytes(data_key, std::as_bytes(std::span(data)),
                                std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: delete the bytestring and create an RCQ at the same key
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_deletion(
        data_key, std::to_underlying(StorageDeltaFlags::None)));
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x01});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        data_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// BugHunt: Two InvokeTXs in the same block each adding an NNInt128 delta
// to the same key. Tests concurrent NNInt128 modifications in one block.
TEST_F(HogDBTest, TwoInvokeTXsSameNNInt128Key)
{
  Address addr = make_address(kAddress + 33);
  ItemKey data_key = make_key(kAddress + 33, 1);
  setup_contract_address(addr);

  // Block 1: two InvokeTXs both modify the same NNInt128
  {
    DbHash base_hash = db_.compute_full_db_hash();

    BlockHeight height = 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());

    // First InvokeTX: add 100
    {
      InvokeTX invoke_tx(block_tx);
      ASSERT_TRUE(invoke_tx.request_nnint128(
          data_key, 0, 100, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    // Second InvokeTX: add 200 more
    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx(block_tx);
      ASSERT_TRUE(invoke_tx.request_nnint128(
          data_key, 200, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    DbHash incremental_hash = *hash_result;
    ASSERT_TRUE(block_tx.get().commit());

    DbHash full_hash = db_.compute_full_db_hash();
    EXPECT_EQ(incremental_hash, full_hash)
        << "Two InvokeTXs modifying same NNInt128 should produce matching hash";
  }
}

// BugHunt: RCQ insert and partial current-block consume in the same block.
// Entries are inserted AND some consumed via current_block_consumed.
TEST_F(HogDBTest, RCQInsertAndCurrentBlockConsumeSameBlock)
{
  Address addr = make_address(kAddress + 34);
  ItemKey rcq_key = make_key(kAddress + 34, 1);
  setup_contract_address(addr);

  // Block 1: insert some entries
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x01});
    delta.insert.added_entries.push_back(QueueEntry{0x02});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: insert new entries AND consume some current-block entries
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x10});
    delta.insert.added_entries.push_back(QueueEntry{0x20});
    delta.insert.added_entries.push_back(QueueEntry{0x30});
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number = 0;
    delta.consume->past_block_consume.partial_consumed_number = 0;
    // Consume one of the current block entries
    delta.consume->current_block_consumed.push_back(QueueEntry{0x20});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// BugHunt: RCQ consume all past blocks AND all current block entries.
// Everything inserted in this block is also consumed, plus past blocks.
TEST_F(HogDBTest, RCQConsumeAllPastAndCurrentEntries)
{
  Address addr = make_address(kAddress + 35);
  ItemKey rcq_key = make_key(kAddress + 35, 1);
  setup_contract_address(addr);

  // Block 1: insert entries
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x01});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: insert new entries, consume all past blocks, consume all current
  // block entries. Net effect: RCQ should be empty.
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0xAA});
    delta.insert.added_entries.push_back(QueueEntry{0xBB});
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number = 1;
    delta.consume->past_block_consume.partial_consumed_number = 0;
    // Consume ALL current block entries
    delta.consume->current_block_consumed.push_back(QueueEntry{0xAA});
    delta.consume->current_block_consumed.push_back(QueueEntry{0xBB});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// BugHunt: Delete RCQ with multiple blocks (explicit deletion).
// The deletion lock must remove all RCQ blocks AND metadata from the hash.
TEST_F(HogDBTest, DeleteRCQWithMultipleBlocks)
{
  Address addr = make_address(kAddress + 38);
  ItemKey rcq_key = make_key(kAddress + 38, 1);
  setup_contract_address(addr);

  // Block 1-3: insert entries across multiple blocks
  for (size_t i = 1; i <= 3; i++) {
    commit_block_expect_db_hash_matches_full(i, [&](InvokeTX &invoke_tx) {
      RetrocausalQueueDelta delta;
      delta.insert.added_entries.push_back(QueueEntry{static_cast<uint8_t>(i)});
      ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
          rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    });
  }

  // Block 4: explicitly delete the RCQ (all 3 blocks + metadata)
  commit_block_expect_db_hash_matches_full(4, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_deletion(
        rcq_key, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// BugHunt: Two InvokeTXs in the same block both adding entries to the same RCQ.
// The combined block must be correctly reflected in the incremental hash.
TEST_F(HogDBTest, TwoInvokeTXsSameRCQ)
{
  Address addr = make_address(kAddress + 41);
  ItemKey rcq_key = make_key(kAddress + 41, 1);
  setup_contract_address(addr);

  {
    DbHash base_hash = db_.compute_full_db_hash();

    BlockHeight height = 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());

    // First InvokeTX: insert entries
    {
      InvokeTX invoke_tx(block_tx);
      RetrocausalQueueDelta delta;
      delta.insert.added_entries.push_back(QueueEntry{0x01});
      delta.insert.added_entries.push_back(QueueEntry{0x02});
      ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
          rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    // Second InvokeTX: insert more entries
    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx(block_tx);
      RetrocausalQueueDelta delta;
      delta.insert.added_entries.push_back(QueueEntry{0x03});
      ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
          rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    DbHash incremental_hash = *hash_result;
    ASSERT_TRUE(block_tx.get().commit());

    DbHash full_hash = db_.compute_full_db_hash();
    EXPECT_EQ(incremental_hash, full_hash)
        << "Two InvokeTXs adding to same RCQ should produce matching hash";
  }
}

// BugHunt: RCQ partial past block consumption. Consume some entries from a
// past block but not all, leaving partial_consumed_number > 0.
TEST_F(HogDBTest, RCQPartialPastBlockConsumption)
{
  Address addr = make_address(kAddress + 42);
  ItemKey rcq_key = make_key(kAddress + 42, 1);
  setup_contract_address(addr);

  // Block 1: insert 3 entries into the first block
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x01});
    delta.insert.added_entries.push_back(QueueEntry{0x02});
    delta.insert.added_entries.push_back(QueueEntry{0x03});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: partially consume block 1 (2 of 3 entries) and insert new entries
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x10});
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number = 0;
    delta.consume->past_block_consume.partial_consumed_number = 2;
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 3: consume the remaining entry from block 1
  commit_block_expect_db_hash_matches_full(3, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x20});
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number = 1;
    delta.consume->past_block_consume.partial_consumed_number = 0;
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });
}

// BugHunt: Multiple replay cache nonces from different InvokeTXs in the
// same block for the same address. Tests concurrent replay cache lock.
TEST_F(HogDBTest, TwoInvokeTXsSameReplayCache)
{
  Address addr = make_address(kAddress + 45);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  {
    DbHash base_hash = db_.compute_full_db_hash();

    BlockHeight height = 1;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());

    // First InvokeTX: add nonce
    {
      InvokeTX invoke_tx(block_tx);
      ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 100),
          std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    // Second InvokeTX: add another nonce at same height
    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx(block_tx);
      ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 200),
          std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    DbHash incremental_hash = *hash_result;
    ASSERT_TRUE(block_tx.get().commit());

    DbHash full_hash = db_.compute_full_db_hash();
    EXPECT_EQ(incremental_hash, full_hash)
        << "Two InvokeTXs adding nonces to same replay cache should match";
  }
}

// BugHunt: RCQ insert in one InvokeTX, consume (including current block
// entries) in a different InvokeTX within the same block. This tests the
// interaction between concurrent insertion and consumption.
TEST_F(HogDBTest, RCQInsertThenConsumeCurrentBlockDiffInvokeTX)
{
  Address addr = make_address(kAddress + 49);
  ItemKey rcq_key = make_key(kAddress + 49, 1);
  setup_contract_address(addr);

  // Block 1: insert some base entries
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{0x01});
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
  });

  // Block 2: two InvokeTXs - one inserts, other consumes (including current
  // block)
  {
    DbHash base_hash = db_.compute_full_db_hash();

    BlockHeight height = 2;
    PerThreadARC<BlockTX> block_tx(db_, cost_model_, background_commit_worker_,
                                   height);
    ASSERT_TRUE(block_tx.try_acquire_reference());

    // First InvokeTX: insert entries
    {
      InvokeTX invoke_tx(block_tx);
      RetrocausalQueueDelta delta;
      delta.insert.added_entries.push_back(QueueEntry{0xAA});
      delta.insert.added_entries.push_back(QueueEntry{0xBB});
      ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
          rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    // Second InvokeTX: consume past block + current block entry
    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx(block_tx);
      RetrocausalQueueDelta delta;
      delta.insert.added_entries.push_back(QueueEntry{0xCC});
      delta.consume.emplace();
      delta.consume->past_block_consume.max_fully_consumed_block_number = 1;
      delta.consume->past_block_consume.partial_consumed_number = 0;
      delta.consume->current_block_consumed.push_back(QueueEntry{0xAA});
      ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
          rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      invoke_tx.commit();
      invoke_tx.release_blocktx_reference();
    }

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    DbHash incremental_hash = *hash_result;
    ASSERT_TRUE(block_tx.get().commit());

    DbHash full_hash = db_.compute_full_db_hash();
    EXPECT_EQ(incremental_hash, full_hash)
        << "RCQ insert then consume current block in different InvokeTXs";
  }
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
