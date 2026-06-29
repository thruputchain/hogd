#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

// xdr types
#include "address_auth.hh"
#include "auth_claim.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
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
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "precompiles.hh"

#include "hogd/base/io.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_age_reconciliation.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/hogexec/common/limits.h"
#include "hogd/init.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
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
      .base_data_item_cost = 12000,
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = 12000,
      .contract_deployment_cost_per_byte = 10,
      .replay_cache_cost_per_entry = 7,
  });

  StorageCostModel cost_model_with_balance_cost_ =
      StorageCostModel(StorageCostModelXdr{
          .ext = ExtensionPoint(0),
          .base_data_item_cost = 12000,
          .bytestring_cost_per_byte = 10,
          .base_contract_deployment_cost = 12000,
          .contract_deployment_cost_per_byte = 10,
          .replay_cache_cost_per_entry = 7,
          .balance_cost = 1000,
      });

  AuthChangeSubscriptionManager subscription_manager_;
  BackgroundCommitWorker background_commit_worker_ =
      BackgroundCommitWorker(subscription_manager_, nullptr);

  template<typename F>
  void commit_block_expect_db_hash_matches_full(BlockHeight height, F &&fn)
  {
    DbHash base_hash = db_.compute_full_db_hash();

    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    fn(invoke_tx);

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
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

    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    auto commit_result = invoke_tx.commit();
    EXPECT_TRUE(commit_result.has_value());
    if (!commit_result.has_value()) {
      return {};
    }
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

// Deterministic non-trivial payload so chunk reassembly errors are visible.
x23::Vec<uint8_t>
make_code_bytes(size_t size)
{
  x23::Vec<uint8_t> buf;
  buf.resize_uninitialized(size);
  for (size_t i = 0; i < size; ++i) {
    buf[i] = static_cast<uint8_t>((i * 31) + 7);
  }
  return buf;
}

// Largest value length that stores inline under the 33-byte wasm-code key.
size_t
max_inline_wasm_code_size()
{
  size_t size = kBigItemThreshold;
  while (!Database::item_fits(make_wasm_code_key(SHA256Hash{}).size(), size)) {
    --size;
  }
  return size;
}

} // namespace

TEST_F(HogDBTest, WasmCodeChunkedRoundtrip)
{
  const size_t max_inline = max_inline_wasm_code_size();
  const std::array<size_t, 5> sizes = {
      max_inline,                          // largest inline row
      max_inline + 1,                      // smallest chunked row
      (2 * kWasmCodeChunkPayloadSize),     // exact multiple of chunk payload
      (2 * kWasmCodeChunkPayloadSize) + 1, // one byte into a final chunk
      kMaxSerializedContractBytes,         // largest row hogdb accepts
  };
  uint8_t which = 0;
  for (size_t size : sizes) {
    SHA256Hash hash{};
    hash[0] = 0xC0;
    hash[1] = which++;
    auto buf = make_code_bytes(size);
    auto buf_span = std::as_bytes(std::span(buf));
    ASSERT_TRUE(db_.write_wasm_code(hash, buf_span)) << size;
    auto rcode = db_.read_wasm_code(hash);
    ASSERT_TRUE(rcode) << size;
    EXPECT_TRUE(std::ranges::equal(buf_span, *rcode)) << size;
    // Re-writing the same row is idempotent (content-addressed rows can be
    // staged again by a later transaction deploying the same contract).
    ASSERT_TRUE(db_.write_wasm_code(hash, buf_span)) << size;
    auto rcode2 = db_.read_wasm_code(hash);
    ASSERT_TRUE(rcode2) << size;
    EXPECT_TRUE(std::ranges::equal(buf_span, *rcode2)) << size;
  }
}

// Guards db_hash compatibility: a row that fits one item must keep the exact
// pre-chunking layout (type 0 at the 33-byte key, no chunk items), so
// chunking changes nothing for existing state.
TEST_F(HogDBTest, WasmCodeSmallRowKeepsLegacyInlineLayout)
{
  SHA256Hash hash{};
  hash[0] = 0xC1;
  auto buf = make_code_bytes(100);
  ASSERT_TRUE(db_.write_wasm_code(hash, std::as_bytes(std::span(buf))));

  auto key = make_wasm_code_key(hash);
  auto type = db_.get_ramdb().get_type(std::as_bytes(std::span(key)));
  ASSERT_TRUE(type);
  EXPECT_EQ(*type, kWasmCodeInline);

  auto chunk_key = make_wasm_code_chunk_key(hash, 0);
  EXPECT_FALSE(db_.get_ramdb().get_type(std::as_bytes(std::span(chunk_key))));
}

TEST_F(HogDBTest, WasmCodeMissingChunkIsCorruption)
{
  SHA256Hash hash{};
  hash[0] = 0xC2;
  auto buf = make_code_bytes(kMaxSerializedContractBytes);
  ASSERT_TRUE(db_.write_wasm_code(hash, std::as_bytes(std::span(buf))));

  auto chunk_key = make_wasm_code_chunk_key(hash, 1);
  ASSERT_TRUE(db_.get_ramdb().remove(std::as_bytes(std::span(chunk_key))));

  auto rcode = db_.read_wasm_code(hash);
  ASSERT_FALSE(rcode);
  EXPECT_EQ(rcode.error().code(), HogDBCode::kCorruption);
}

// The chunked-head sanity checks (unknown type byte, fits-inline total,
// out-of-range chunk count) all reject with kCorruption -- and they run
// before read_wasm_code allocates `total` bytes, so a hostile head cannot
// drive a huge allocation (the 0xFFFFFFFF case would be a 4 GiB buffer).
TEST_F(HogDBTest, WasmCodeTamperedHeadIsCorruption)
{
  SHA256Hash hash{};
  hash[0] = 0xC3;
  auto buf = make_code_bytes(kMaxSerializedContractBytes);
  ASSERT_TRUE(db_.write_wasm_code(hash, std::as_bytes(std::span(buf))));

  auto key = make_wasm_code_key(hash);
  auto key_span = std::as_bytes(std::span(key));

  auto expect_corrupt_after = [&](uint8_t type,
                                  std::span<const std::byte> value) {
    ASSERT_TRUE(db_.get_ramdb().put(key_span, type, value));
    auto rcode = db_.read_wasm_code(hash);
    ASSERT_FALSE(rcode);
    EXPECT_EQ(rcode.error().code(), HogDBCode::kCorruption);
  };

  // Unknown head type byte.
  auto valid_len = x23::serialize(static_cast<uint32_t>(buf.size()));
  expect_corrupt_after(7, std::as_bytes(std::span(valid_len)));

  // Chunked head describing a row small enough to be stored inline.
  auto small_len = x23::serialize(static_cast<uint32_t>(100));
  expect_corrupt_after(kWasmCodeChunkedHead,
                       std::as_bytes(std::span(small_len)));

  // Chunked head describing more chunks than a one-byte index addresses.
  auto huge_len = x23::serialize(static_cast<uint32_t>(0xFFFFFFFF));
  expect_corrupt_after(kWasmCodeChunkedHead,
                       std::as_bytes(std::span(huge_len)));
}

// for_each_wasm_code_item emits payload chunks before the chunked head, so a
// reader that observes a head always finds complete chunks. Pin that
// ordering: it is documented behavior the read path relies on.
TEST_F(HogDBTest, WasmCodeChunkedItemsEmitHeadLast)
{
  SHA256Hash hash{};
  hash[0] = 0xC4;
  auto buf = make_code_bytes((2 * kWasmCodeChunkPayloadSize) + 1);

  std::vector<std::pair<size_t, uint8_t>> emitted; // (key length, type)
  for_each_wasm_code_item(hash, std::as_bytes(std::span(buf)),
                          [&](std::span<const std::byte> key, uint8_t type,
                              std::span<const std::byte> /*value*/) {
                            emitted.emplace_back(key.size(), type);
                          });

  ASSERT_EQ(emitted.size(), 4U); // 3 chunks + head
  for (size_t i = 0; i + 1 < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].first, sizeof(SHA256Hash) + 2) << i;
    EXPECT_EQ(emitted[i].second, kWasmCodeChunk) << i;
  }
  EXPECT_EQ(emitted.back().first, sizeof(SHA256Hash) + 1);
  EXPECT_EQ(emitted.back().second, kWasmCodeChunkedHead);
}

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

TEST_F(HogDBTest, NegativeBaseConcurrentAddsPersistMaxValue)
{
  constexpr int128_t kBaseValue = -100;
  ItemKey item_key = make_key(kAddress + 22, 7);
  LockContext ctx(&db_, BlockHeight{1}, item_key, &cost_model_);
  NNInt128Lock lock(kBaseValue);

  NNInt128 large_add(kBaseValue, std::numeric_limits<int128_t>::max());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &large_add, ctx));

  NNInt128 small_add(kBaseValue, 100);
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &small_add, ctx));

  auto changes = db_.create_changeset();
  ASSERT_TRUE(lock.stage_committed_changes(ctx, changes));
  ASSERT_TRUE(db_.apply(changes));

  auto reread_value = db_.read_nnint128(item_key);
  ASSERT_TRUE(reread_value);
  EXPECT_EQ(*reread_value, std::numeric_limits<int128_t>::max());
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
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, height,
      BlockExecutionMode::kBlockExecutionModeProposal);

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
  ASSERT_TRUE(invoke_tx.commit());
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

    // height is now 10, 101 expired, and it shouldn't be possible to re-add it
    ItemChange c2(ReplayCacheIt(Uniquifier(height, 101)));
    auto r = lock2.try_atomic_change_lock(nullptr, &c2, ctx);
    ASSERT_FALSE(r);

    ItemChange c3(ReplayCacheIt(Uniquifier(height + 1, 102)));
    auto r2 = lock2.try_atomic_change_lock(nullptr, &c3, ctx);
    ASSERT_TRUE(r2);
    EXPECT_EQ(cost_model_.kReplayCacheCostPerEntry, r2.value());

    auto commit_cost = lock2.commit_change(c3, ctx);
    ASSERT_TRUE(commit_cost.has_value());
    EXPECT_EQ(cost_model_.kReplayCacheCostPerEntry, *commit_cost);
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

TEST_F(HogDBTest, ReplayCacheMaxExpiryHeightStageAndApply)
{
  // Regression for issue #1447: stage_committed_changes must not overflow
  // when an active replay-cache height equals INT64_MAX.
  constexpr BlockHeight kMaxHeight = std::numeric_limits<BlockHeight>::max();
  BlockHeight current_height = 9;
  auto rckey = make_replay_cache_item_key(make_address("abcd"));

  ItemChange c(ReplayCacheIt(Uniquifier(kMaxHeight, 7)));
  ItemLock lock(0);

  LockContext ctx(&db_, current_height, rckey, &cost_model_);
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &c, ctx));
  ASSERT_TRUE(lock.commit_change(c, ctx).has_value());

  auto changes = db_.create_changeset();
  ASSERT_TRUE(lock.stage_committed_changes(ctx, changes));
  ASSERT_TRUE(db_.apply(changes));

  auto data =
      db_.read_address_replay_cache_data(rckey.get_address(), kMaxHeight);
  ASSERT_TRUE(data);
  ASSERT_EQ(1U, data->nonces.size());
  EXPECT_EQ(7U, data->nonces[0]);

  auto meta = db_.read_address_replay_cache_meta(rckey.get_address());
  ASSERT_TRUE(meta);
  EXPECT_EQ(1, meta->size);
  ASSERT_EQ(1U, meta->heights.size());
  EXPECT_EQ(kMaxHeight, meta->heights[0].height);
  EXPECT_EQ(1U, meta->heights[0].nonce_count);
}

TEST_F(HogDBTest, BalanceTransferReducesByCost)
{
  Address sender_addr = make_address(kAddress);
  Address recipient_addr = make_address(kAddress + 1);

  // Set initial balance for sender
  constexpr int128_t kInitialBalance = 100000;
  ASSERT_TRUE(db_.write_address_balance(sender_addr, kInitialBalance));

  BlockHeight height = 1;
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, height,
      BlockExecutionMode::kBlockExecutionModeProposal);

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
  ASSERT_TRUE(invoke_tx.commit());
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

TEST_F(HogDBTest, NewBalanceHogAgeIsAllZeroes)
{
  Address sender_addr = make_address(kAddress);
  Address recipient_addr = make_address(kAddress + 1);

  constexpr int128_t kInitialBalance = 100000;
  ASSERT_TRUE(db_.write_address_balance(sender_addr, kInitialBalance));

  constexpr BlockHeight kBlockHeight = 42;
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, kBlockHeight,
      BlockExecutionMode::kBlockExecutionModeProposal);

  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

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
  ASSERT_TRUE(invoke_tx.commit());
  invoke_tx.release_blocktx_reference();

  ASSERT_TRUE(block_tx.get().close(DbHash{}));
  ASSERT_TRUE(block_tx.get().commit());

  auto age = db_.read_address_average_hog_age(recipient_addr);
  ASSERT_TRUE(age);

  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0U);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 0UL);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), 0U);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 0UL);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]), 0U);
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], 0UL);
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
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, height,
      BlockExecutionMode::kBlockExecutionModeProposal);

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
    ASSERT_TRUE(invoke_tx1.commit());
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
    ASSERT_TRUE(invoke_tx2.commit());
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
    args.new_required_weight = Weight{0};
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
  auto db_key = make_contract_data_key(key);
  auto balance_key = make_address_key(contract_addr, Prefix::kBalance);
  DbHash base_hash = db_.compute_full_db_hash();
  constexpr int128_t kInitialBalance = 10'000'000;

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  ASSERT_TRUE(
      invoke_tx.request_nnint128(key, 321,
                                 std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  ASSERT_TRUE(invoke_tx.commit());
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

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  ASSERT_TRUE(
      invoke_tx.request_nnint128(key, 321,
                                 std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(invoke_tx.sync_balance_locks());
  ASSERT_TRUE(invoke_tx.commit());
  invoke_tx.release_blocktx_reference();

  auto hash_result = block_tx.get().close(base_hash);
  ASSERT_TRUE(hash_result.has_value());
  ASSERT_TRUE(block_tx.get().commit());

  EXPECT_TRUE(db_.check_invariants());
}

TEST_F(HogDBTest, ApplyFeedsDebugInvariants)
{
  // Regression guard against the debug invariants being silently disconnected
  // from the commit path. Block finalization flushes committed item changes
  // through HogDBDAO::apply, which must feed the invariants the before/after
  // state of every change. When that feed is severed (as it was after
  // finalization moved to ChangeSet/apply and stopped routing through the
  // typed write_* methods), the invariants observe nothing and
  // check_invariants() trivially returns true regardless of the state.
  //
  // We stage a balance creation with no offsetting transfer -- a deliberate
  // conservation violation -- and flush it through apply(), the exact path
  // block commit uses. The invariant must observe it and report the violation.
  // If the feed is ever disconnected, total_supply_change_ stays zero and this
  // EXPECT_FALSE fails.
  //
  // The invariants are only fed in debug builds (the feed is gated on
  // is_debug_build()), so the violation is only observable there.
  if (!is_debug_build()) {
    GTEST_SKIP() << "debug invariants are only fed in debug builds";
  }

  db_.enable_debug_invariants(cost_model_);

  Address addr = make_address(kAddress + 71);
  auto balance_key = make_address_key(addr, Prefix::kBalance);
  auto balance_bytes = int128_to_xdr_bytes(1000);
  std::span<const std::byte> key_span = std::as_bytes(std::span(balance_key));
  std::span<const std::byte> value_span =
      std::as_bytes(std::span(balance_bytes));

  auto changeset = db_.create_changeset();
  changeset.put(key_span, kNNInt128, value_span);
  ASSERT_TRUE(db_.apply(changeset));

  EXPECT_FALSE(db_.check_invariants());
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

// A contract row bigger than one RAMDB item commits chunked: the staged
// chunk items' incremental db_hash delta matches a full state-hash
// recompute, and the DAO reassembles the row byte-identically.
TEST_F(HogDBTest, DbHashDeltaChunkedNewContractLockMatchesFullStateHash)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract().data.resize_uninitialized(2 * kBigItemThreshold);
  for (size_t i = 0; i < contract.wasmi_contract().data.size(); ++i) {
    contract.wasmi_contract().data[i] = static_cast<uint8_t>((i * 31) + 7);
  }

  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_new_contract(contract));
  });

  auto rcode = db_.read_wasm_code(sha256(contract));
  ASSERT_TRUE(rcode);
  auto expected = x23::serialize(contract);
  EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(expected)), *rcode));
}

// Regression for #1775/#2569: a serialized contract row larger than
// kMaxSerializedContractBytes is executor misbehavior -- hogexec enforces
// kMaxContractSizeBytes on attached blobs before execution, and hogdb stores
// any legal row (chunked if necessary). Reject at staging with
// kInvalidExecutorBehavior, not at block write-back, where apply() would
// leave earlier writes visible and the commit error would abort the node.
TEST_F(HogDBTest, NewContractExceedingSerializedBoundIsInvalidExecutorBehavior)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract().data.resize_uninitialized(
      kMaxSerializedContractBytes + 1);

  // A row at exactly the bound is accepted (the preflight is >, not >=).
  // Blob of (bound - 8) serializes to exactly kMaxSerializedContractBytes:
  // 4-byte union tag + 4-byte opaque length + data (multiple of 4, no pad).
  Contract at_bound_contract(ContractType::kWasmi);
  at_bound_contract.wasmi_contract().data.resize_uninitialized(
      kMaxSerializedContractBytes - 8);
  ASSERT_EQ(x23::xdr_size(at_bound_contract), kMaxSerializedContractBytes);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  ASSERT_TRUE(invoke_tx.request_new_contract(at_bound_contract));

  auto ret = invoke_tx.request_new_contract(contract);
  ASSERT_FALSE(ret);
  EXPECT_EQ(HogDBCode::kInvalidExecutorBehavior, ret.error().code());

  invoke_tx.abort();
  invoke_tx.release_blocktx_reference();
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);

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
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  DbHash db_hash_before_consume = db_.compute_full_db_hash();

  // Consume the first block completely
  DbHash db_hash_after_consume;
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    RetrocausalQueueDelta delta;
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number = 1;
    delta.consume->past_block_consume.partial_consumed_number = 0;

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  DbHash db_hash_before_delete = db_.compute_full_db_hash();

  // Delete the entire RCQ by consuming all entries
  DbHash db_hash_after_delete;
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_deletion(
        rcq_key, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_bytes(
        rcq_key, std::span<const std::byte>(std::as_bytes(std::span("test"))),
        std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_nnint128(
        rcq_key, 100, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Fully consume all entries
  {
    BlockHeight height = kNumBlocks + 1;
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, 5,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, 5,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, 1,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    AuthDataChangeArgs args{};
    args.new_required_weight = Weight{0};
    ASSERT_TRUE(invoke_tx.request_auth_data(
        addr, args, std::to_underlying(StorageDeltaFlags::None)));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
    invoke_tx.release_blocktx_reference();
    ASSERT_TRUE(block_tx.get().close(DbHash{}));
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Block 2 on db_: delete the auth data
  {
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, 2,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_deletion(
        make_address_auth_data_item_key(addr),
        std::to_underlying(StorageDeltaFlags::None)));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
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

  // Block 1: create auth data with required weight
  commit_block_expect_db_hash_matches_full(1, [&](InvokeTX &invoke_tx) {
    AuthDataChangeArgs args{};
    args.new_required_weight = Weight{0};
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());

    // First InvokeTX: add 100
    {
      InvokeTX invoke_tx(block_tx);
      ASSERT_TRUE(invoke_tx.request_nnint128(
          data_key, 0, 100, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      ASSERT_TRUE(invoke_tx.commit());
      invoke_tx.release_blocktx_reference();
    }

    // Second InvokeTX: add 200 more
    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx(block_tx);
      ASSERT_TRUE(invoke_tx.request_nnint128(
          data_key, 200, std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
      ASSERT_TRUE(invoke_tx.commit());
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
      ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx.try_acquire_reference());

    // First InvokeTX: add nonce
    {
      InvokeTX invoke_tx(block_tx);
      ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 100),
          std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx.sync_balance_locks());
      ASSERT_TRUE(invoke_tx.commit());
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
      ASSERT_TRUE(invoke_tx.commit());
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
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);
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
      ASSERT_TRUE(invoke_tx.commit());
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
      ASSERT_TRUE(invoke_tx.commit());
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

// Regression for #1458: aborting the only reservation on a fresh expiry
// height must not leave phantom zero-count metadata or an empty data row.
TEST_F(HogDBTest, AbortedFreshHeightDoesNotPersistEmptyReplayCacheHeight)
{
  Address addr = make_address(kAddress + 53);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  {
    DbHash base_hash = db_.compute_full_db_hash();

    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, 1,
        BlockExecutionMode::kBlockExecutionModeProposal);

    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx_a(block_tx);
    ASSERT_TRUE(invoke_tx_a.request_consume_uniquifier(
        addr, Uniquifier(99999, 100),
        std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx_a.sync_balance_locks());

    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx_b(block_tx);
      ASSERT_TRUE(invoke_tx_b.request_consume_uniquifier(
          addr, Uniquifier(3, 200),
          std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx_b.sync_balance_locks());
      ASSERT_TRUE(invoke_tx_b.commit());
      invoke_tx_b.release_blocktx_reference();
    }

    invoke_tx_a.abort();
    invoke_tx_a.release_blocktx_reference();

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    ASSERT_TRUE(block_tx.get().commit());
  }

  auto meta = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(meta);
  EXPECT_EQ(1, meta->size);
  ASSERT_EQ(1U, meta->heights.size());
  EXPECT_EQ(3, meta->heights[0].height);
  EXPECT_EQ(1U, meta->heights[0].nonce_count);

  auto committed_data = db_.read_address_replay_cache_data(addr, 3);
  ASSERT_TRUE(committed_data);
  ASSERT_EQ(1U, committed_data->nonces.size());
  EXPECT_EQ(200U, committed_data->nonces[0]);

  EXPECT_FALSE(db_.read_address_replay_cache_data(addr, 99999).has_value());
}

// The phantom must stay absent after a subsequent block reloads and
// rewrites the address's replay-cache metadata.
TEST_F(HogDBTest, AbortedFreshHeightStaysAbsentInNextBlock)
{
  Address addr = make_address(kAddress + 54);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  {
    DbHash base_hash = db_.compute_full_db_hash();

    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, 1,
        BlockExecutionMode::kBlockExecutionModeProposal);

    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx_a(block_tx);
    ASSERT_TRUE(invoke_tx_a.request_consume_uniquifier(
        addr, Uniquifier(99999, 100),
        std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx_a.sync_balance_locks());

    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx_b(block_tx);
      ASSERT_TRUE(invoke_tx_b.request_consume_uniquifier(
          addr, Uniquifier(3, 200),
          std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx_b.sync_balance_locks());
      ASSERT_TRUE(invoke_tx_b.commit());
      invoke_tx_b.release_blocktx_reference();
    }

    invoke_tx_a.abort();
    invoke_tx_a.release_blocktx_reference();

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    ASSERT_TRUE(block_tx.get().commit());
  }

  // Block 2 touches the replay cache on the same address for a different
  // fresh expiry height. Forces the lock to reload metadata from DB and
  // re-stage it — any leaked 99999 entry would be carried forward here.
  commit_block_expect_db_hash_matches_full(2, [&](InvokeTX &invoke_tx) {
    ASSERT_TRUE(invoke_tx.request_consume_uniquifier(
        addr, Uniquifier(77777, 900),
        std::to_underlying(StorageDeltaFlags::None)));
  });

  auto meta = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(meta);
  EXPECT_EQ(2, meta->size);
  ASSERT_EQ(2U, meta->heights.size());
  EXPECT_EQ(3, meta->heights[0].height);
  EXPECT_EQ(1U, meta->heights[0].nonce_count);
  EXPECT_EQ(77777, meta->heights[1].height);
  EXPECT_EQ(1U, meta->heights[1].nonce_count);

  EXPECT_FALSE(db_.read_address_replay_cache_data(addr, 99999).has_value());
}

// Positive control: aborting one of two reservations on the same fresh
// expiry height must NOT remove the height — only the aborted nonce.
TEST_F(HogDBTest, AbortedOneOfTwoFreshReservationsKeepsFreshHeight)
{
  Address addr = make_address(kAddress + 55);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  {
    DbHash base_hash = db_.compute_full_db_hash();

    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_, background_commit_worker_, 1,
        BlockExecutionMode::kBlockExecutionModeProposal);

    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx_a(block_tx);
    ASSERT_TRUE(invoke_tx_a.request_consume_uniquifier(
        addr, Uniquifier(99999, 100),
        std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(invoke_tx_a.sync_balance_locks());

    ASSERT_TRUE(block_tx.try_acquire_reference());
    {
      InvokeTX invoke_tx_b(block_tx);
      ASSERT_TRUE(invoke_tx_b.request_consume_uniquifier(
          addr, Uniquifier(99999, 200),
          std::to_underlying(StorageDeltaFlags::None)));
      ASSERT_TRUE(invoke_tx_b.sync_balance_locks());
      ASSERT_TRUE(invoke_tx_b.commit());
      invoke_tx_b.release_blocktx_reference();
    }

    invoke_tx_a.abort();
    invoke_tx_a.release_blocktx_reference();

    auto hash_result = block_tx.get().close(base_hash);
    ASSERT_TRUE(hash_result.has_value());
    ASSERT_TRUE(block_tx.get().commit());
  }

  auto meta = db_.read_address_replay_cache_meta(addr);
  ASSERT_TRUE(meta);
  EXPECT_EQ(1, meta->size);
  ASSERT_EQ(1U, meta->heights.size());
  EXPECT_EQ(99999, meta->heights[0].height);
  EXPECT_EQ(1U, meta->heights[0].nonce_count);

  auto data = db_.read_address_replay_cache_data(addr, 99999);
  ASSERT_TRUE(data);
  ASSERT_EQ(1U, data->nonces.size());
  EXPECT_EQ(200U, data->nonces[0]);
}

// Strongest regression: a block that reserves-and-aborts a fresh expiry
// height must leave the DB in a state bit-identical to a block that never
// attempted the aborted reservation at all.
TEST_F(HogDBTest, AbortedFreshHeightHashEquivalentToCleanBlock)
{
  auto run_scenario = [&](bool include_aborted_attempt) -> DbHash {
    ErrnoStatus<Directory> dir = Directory::open_temporary();
    EXPECT_TRUE(static_cast<bool>(dir));
    constexpr size_t kHeapSize = 10_MiB;
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), Database::estimate_heap_size(kHeapSize),
                       0);
    EXPECT_TRUE(db);
    HogDBDAO dao(RAMDB(std::move(*db)));

    Address addr = make_address(kAddress + 56);
    EXPECT_TRUE(dao.write_address_balance(addr, 10'000'000));

    DbHash base_hash = dao.compute_full_db_hash();

    PerThreadARC<BlockTX> block_tx(
        dao, cost_model_, background_commit_worker_, 1,
        BlockExecutionMode::kBlockExecutionModeProposal);

    if (include_aborted_attempt) {
      EXPECT_TRUE(block_tx.try_acquire_reference());
      InvokeTX invoke_tx_a(block_tx);
      EXPECT_TRUE(invoke_tx_a.request_consume_uniquifier(
          addr, Uniquifier(99999, 100),
          std::to_underlying(StorageDeltaFlags::None)));
      EXPECT_TRUE(invoke_tx_a.sync_balance_locks());

      EXPECT_TRUE(block_tx.try_acquire_reference());
      {
        InvokeTX invoke_tx_b(block_tx);
        EXPECT_TRUE(invoke_tx_b.request_consume_uniquifier(
            addr, Uniquifier(3, 200),
            std::to_underlying(StorageDeltaFlags::None)));
        EXPECT_TRUE(invoke_tx_b.sync_balance_locks());
        EXPECT_TRUE(invoke_tx_b.commit());
        invoke_tx_b.release_blocktx_reference();
      }

      invoke_tx_a.abort();
      invoke_tx_a.release_blocktx_reference();
    }
    else {
      EXPECT_TRUE(block_tx.try_acquire_reference());
      InvokeTX invoke_tx(block_tx);
      EXPECT_TRUE(invoke_tx.request_consume_uniquifier(
          addr, Uniquifier(3, 200),
          std::to_underlying(StorageDeltaFlags::None)));
      EXPECT_TRUE(invoke_tx.sync_balance_locks());
      EXPECT_TRUE(invoke_tx.commit());
      invoke_tx.release_blocktx_reference();
    }

    auto hash_result = block_tx.get().close(base_hash);
    EXPECT_TRUE(hash_result.has_value());
    EXPECT_TRUE(block_tx.get().commit());

    return dao.compute_full_db_hash();
  };

  DbHash hash_with_aborted = run_scenario(true);
  DbHash hash_without_aborted = run_scenario(false);
  EXPECT_EQ(hash_with_aborted, hash_without_aborted);
}

TEST_F(HogDBTest, ReleaseBalanceRefundOnFreshAddressCrossTx)
{
  Address addr = make_address(kAddress + 99);
  ItemKey rcq_key = make_key(kAddress + 99, 1);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  // T1: fund the fresh address and insert RCQ entries
  ASSERT_TRUE(block_tx.try_acquire_reference());
  {
    InvokeTX itx(block_tx);
    ASSERT_TRUE(
        itx.request_nnint128(make_address_balance_item_key(addr), 100'000,
                             std::to_underlying(StorageDeltaFlags::None)));

    RetrocausalQueueDelta delta;
    for (uint8_t i = 0; i < 10; ++i) {
      delta.insert.added_entries.push_back(QueueEntry{i});
    }
    ASSERT_TRUE(itx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));

    ASSERT_TRUE(itx.sync_balance_locks());
    ASSERT_TRUE(itx.commit());
    itx.release_blocktx_reference();
  }

  // T2: consume RCQ entries, producing a small storage refund
  ASSERT_TRUE(block_tx.try_acquire_reference());
  {
    InvokeTX itx(block_tx);
    RetrocausalQueueDelta delta;
    delta.consume.emplace();
    for (uint8_t i = 0; i < 5; ++i) {
      delta.consume->current_block_consumed.push_back(QueueEntry{i});
    }
    ASSERT_TRUE(itx.request_retrocausal_queue(
        rcq_key, delta, std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(itx.sync_balance_locks());

    // Before the fix, this throws std::runtime_error("failed to lock balance")
    ASSERT_TRUE(itx.commit());
    itx.release_blocktx_reference();
  }
}

TEST_F(HogDBTest, ReleaseBalanceRefundOnFreshAddressSameTx)
{
  Address addr = make_address(kAddress + 100);
  ItemKey rcq_key = make_key(kAddress + 100, 1);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  // Single tx: fund address, insert RCQ entries, then add a consume in the
  // same invocation via a child frame. The cumulative delta (inserts + consume)
  // narrows the RCQ reservation, producing a refund at commit time when the
  // conservative lock overestimates the actual cost. Without two-phase commit
  // ordering, the RCQ key may be committed before the balance key, hitting
  // kUncommitted state.
  ASSERT_TRUE(block_tx.try_acquire_reference());
  {
    InvokeTX itx(block_tx);
    ASSERT_TRUE(
        itx.request_nnint128(make_address_balance_item_key(addr), 100'000,
                             std::to_underlying(StorageDeltaFlags::None)));

    RetrocausalQueueDelta insert_delta;
    for (uint8_t i = 0; i < 10; ++i) {
      insert_delta.insert.added_entries.push_back(QueueEntry{i});
    }
    ASSERT_TRUE(itx.request_retrocausal_queue(
        rcq_key, insert_delta, std::to_underlying(StorageDeltaFlags::None)));

    ASSERT_TRUE(itx.sync_balance_locks());

    // Child frame: cumulative delta keeps all inserts and adds a consume.
    // narrow() requires the new delta to be a superset of the earlier one.
    [[maybe_unused]] int sp = itx.create_savepoint();

    RetrocausalQueueDelta cumulative_delta;
    for (uint8_t i = 0; i < 10; ++i) {
      cumulative_delta.insert.added_entries.push_back(QueueEntry{i});
    }
    cumulative_delta.consume.emplace();
    for (uint8_t i = 0; i < 5; ++i) {
      cumulative_delta.consume->current_block_consumed.push_back(QueueEntry{i});
    }
    ASSERT_TRUE(itx.request_retrocausal_queue(rcq_key, cumulative_delta,
                                              std::to_underlying(
                                                  StorageDeltaFlags::None)));
    ASSERT_TRUE(itx.sync_balance_locks());

    ASSERT_TRUE(itx.commit());
    itx.release_blocktx_reference();
  }
}

TEST(InvokeTxKeyTest, HashAndEquality)
{
  hogd::InvokeTxKey k1{.block_tx_id = 17, .invoke_tx_id = 29};
  hogd::InvokeTxKey k2{.block_tx_id = 17, .invoke_tx_id = 29};
  hogd::InvokeTxKey k3{.block_tx_id = 17, .invoke_tx_id = 30};

  EXPECT_EQ(k1, k2);
  EXPECT_NE(k1, k3);

  hogd::InvokeTxKey::Hash hasher;
  EXPECT_EQ(hasher(k1), hasher(k2));
  EXPECT_EQ(hasher(k1), std::hash<hogd::BlockTXID>()(k1.block_tx_id) ^
                            std::hash<hogd::InvokeTXID>()(k1.invoke_tx_id));
}

TEST(InvokeTxKeyTest, SynchronizedMapRoundTrip)
{
  hogd::SynchronizedMap<hogd::InvokeTxKey, int, hogd::InvokeTxKey::Hash> map(2);
  hogd::InvokeTxKey key{.block_tx_id = 42, .invoke_tx_id = 99};

  {
    auto [value, lock] = map[key];
    value = 1234;
  }
  {
    auto [value, lock] = map.find(key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 1234);
  }
  EXPECT_EQ(map.remove(key), 1U);
  {
    auto [value, lock] = map.find(key);
    EXPECT_EQ(value, nullptr);
  }
}

// Tests that the fresh-balance guard in BalanceLock::try_atomic_change_lock
// is evaluated per-lock-attempt, not against cumulative committed state.
// A cumulative guard would be nondeterministic under concurrent InvokeTX
// instances (see #1606 discussion — option 1 was rejected for this reason).

TEST_F(HogDBTest, FreshBalanceGuardRejectsUnderfundedTxRegardlessOfPeerState)
{
  const int128_t kBalanceCost =
      cost_model_with_balance_cost_.get_balance_cost();

  Address addr = make_address(kAddress + 200);
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_with_balance_cost_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  // T1: fund the fresh address with enough to cover the balance-row cost.
  ASSERT_TRUE(block_tx.try_acquire_reference());
  {
    InvokeTX itx(block_tx);
    ASSERT_TRUE(
        itx.request_nnint128(make_address_balance_item_key(addr), kBalanceCost,
                             std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(itx.sync_balance_locks());
    ASSERT_TRUE(itx.commit());
    itx.release_blocktx_reference();
  }

  // T2: fund the same fresh address with 1 token (below balance-row cost).
  // This must always fail, regardless of T1's committed state.
  // Use savepoint so we can rollback on failure and keep InvokeTX consistent.
  ASSERT_TRUE(block_tx.try_acquire_reference());
  {
    InvokeTX itx(block_tx);
    int depth = itx.get_savepoint_head();
    itx.create_savepoint();
    ASSERT_TRUE(
        itx.request_nnint128(make_address_balance_item_key(addr), 1,
                             std::to_underlying(StorageDeltaFlags::None)));
    EXPECT_FALSE(itx.sync_balance_locks());
    itx.rollback_to_savepoint(depth);
    itx.release_blocktx_reference();
  }
}

TEST_F(HogDBTest, FreshBalanceGuardAbortDoesNotInvalidatePeerLock)
{
  const int128_t kBalanceCost =
      cost_model_with_balance_cost_.get_balance_cost();

  Address addr = make_address(kAddress + 201);
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_with_balance_cost_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  // T1: fund the fresh address with exactly the balance-row cost.
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX itx1(block_tx);
  ASSERT_TRUE(
      itx1.request_nnint128(make_address_balance_item_key(addr), kBalanceCost,
                            std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(itx1.sync_balance_locks());

  // T2: underfunded — must fail even though T1 has a pending lock.
  ASSERT_TRUE(block_tx.try_acquire_reference());
  {
    InvokeTX itx2(block_tx);
    int depth = itx2.get_savepoint_head();
    itx2.create_savepoint();
    ASSERT_TRUE(
        itx2.request_nnint128(make_address_balance_item_key(addr), 1,
                              std::to_underlying(StorageDeltaFlags::None)));
    EXPECT_FALSE(itx2.sync_balance_locks());
    itx2.rollback_to_savepoint(depth);
    itx2.release_blocktx_reference();
  }

  // T1 aborts. If a cumulative guard had let T2 through, the balance row
  // would now have insufficient funds. T1's abort must not retroactively
  // invalidate any accepted locks (and T2 was correctly rejected above).
  itx1.abort();
  itx1.release_blocktx_reference();
}

TEST_F(HogDBTest, FreshBalanceGuardDeterministicAcrossCommitOrder)
{
  const int128_t kBalanceCost =
      cost_model_with_balance_cost_.get_balance_cost();

  Address addr = make_address(kAddress + 202);

  // Run the same scenario twice with different commit orderings.
  // An underfunded T2 must fail in both orderings.
  for (int ordering = 0; ordering < 2; ++ordering) {
    BlockHeight height = ordering + 1;
    PerThreadARC<BlockTX> block_tx(
        db_, cost_model_with_balance_cost_, background_commit_worker_, height,
        BlockExecutionMode::kBlockExecutionModeProposal);

    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX itx1(block_tx);
    ASSERT_TRUE(
        itx1.request_nnint128(make_address_balance_item_key(addr), kBalanceCost,
                              std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(itx1.sync_balance_locks());

    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX itx2(block_tx);

    if (ordering == 0) {
      // T1 commits before T2 tries to lock balance
      ASSERT_TRUE(itx1.commit());
      itx1.release_blocktx_reference();

      int depth = itx2.get_savepoint_head();
      itx2.create_savepoint();
      ASSERT_TRUE(
          itx2.request_nnint128(make_address_balance_item_key(addr), 1,
                                std::to_underlying(StorageDeltaFlags::None)));
      EXPECT_FALSE(itx2.sync_balance_locks())
          << "underfunded T2 must fail even after T1 committed";
      itx2.rollback_to_savepoint(depth);
      itx2.release_blocktx_reference();
    }
    else {
      // T2 tries to lock balance before T1 commits
      int depth = itx2.get_savepoint_head();
      itx2.create_savepoint();
      ASSERT_TRUE(
          itx2.request_nnint128(make_address_balance_item_key(addr), 1,
                                std::to_underlying(StorageDeltaFlags::None)));
      EXPECT_FALSE(itx2.sync_balance_locks())
          << "underfunded T2 must fail even before T1 committed";
      itx2.rollback_to_savepoint(depth);
      itx2.release_blocktx_reference();

      ASSERT_TRUE(itx1.commit());
      itx1.release_blocktx_reference();
    }
  }
}

TEST_F(HogDBTest, AuthDataNarrowReturnsStaleBlockOnBaseMismatch)
{
  Address addr = make_address(kAddress + 300);
  ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));

  PerThreadARC<BlockTX> block_tx_a(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx_a.try_acquire_reference());
  InvokeTX itx_a(block_tx_a);

  AuthDataChangeArgs first_args{};
  first_args.new_required_weight = Weight{0};
  ASSERT_TRUE(
      itx_a.request_auth_data(addr, first_args,
                              std::to_underlying(StorageDeltaFlags::None)));
  ASSERT_TRUE(itx_a.sync_balance_locks());

  {
    PerThreadARC<BlockTX> block_tx_b(
        db_, cost_model_, background_commit_worker_, 1,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(block_tx_b.try_acquire_reference());
    InvokeTX itx_b(block_tx_b);

    AuthDataChangeArgs sibling_args{};
    PublicKey key;
    key.ed25519().fill(0xBB);
    sibling_args.auth_data_change_signers.push_back(AuthDataChangeSigner{
        .public_key_id = 0,
        .public_key = x23::Ptr<PublicKeyConfig>(
            new PublicKeyConfig{.key = key, .max_weight = 100})});
    ASSERT_TRUE(
        itx_b.request_auth_data(addr, sibling_args,
                                std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(itx_b.sync_balance_locks());
    ASSERT_TRUE(itx_b.commit());
    itx_b.release_blocktx_reference();
    ASSERT_TRUE(block_tx_b.get().close(DbHash{}));
    ASSERT_TRUE(block_tx_b.get().commit());
  }

  AuthDataChangeArgs second_args{};
  second_args.new_required_weight = Weight{1};
  auto result =
      itx_a.request_auth_data(addr, second_args,
                              std::to_underlying(StorageDeltaFlags::None));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), HogDBCode::kStaleBlock);

  itx_a.abort();
  itx_a.release_blocktx_reference();
}

TEST_F(HogDBTest, InvalidateSiblingBlockTX)
{
  PerThreadARC<BlockTX> block_a(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  PerThreadARC<BlockTX> block_b(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  EXPECT_FALSE(block_a.get().is_invalidated());
  EXPECT_FALSE(block_b.get().is_invalidated());

  block_b.get().invalidate();

  EXPECT_FALSE(block_a.get().is_invalidated());
  EXPECT_TRUE(block_b.get().is_invalidated());
}

TEST_F(HogDBTest, InvalidatedBlockTXReadReturnsStaleBlock)
{
  constexpr uint64_t kInvalidAddr = 0xABCD0001;
  Address addr = make_address(kInvalidAddr);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  ItemKey key = make_item_key(addr, 0);
  auto result_before = block_tx.get().read_nnint128_base(key);
  ASSERT_TRUE(result_before.has_value());

  block_tx.get().invalidate();

  auto result_after = block_tx.get().read_nnint128_base(key);
  ASSERT_FALSE(result_after.has_value());
  EXPECT_EQ(result_after.error().code(), HogDBCode::kStaleBlock);
}

TEST_F(HogDBTest, InvalidatedBlockTXBalanceReadReturnsStaleBlock)
{
  constexpr uint64_t kInvalidAddr = 0xABCD0002;
  Address addr = make_address(kInvalidAddr);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  auto result_before = block_tx.get().read_balance_base(addr);
  ASSERT_TRUE(result_before.has_value());

  block_tx.get().invalidate();

  auto result_after = block_tx.get().read_balance_base(addr);
  ASSERT_FALSE(result_after.has_value());
  EXPECT_EQ(result_after.error().code(), HogDBCode::kStaleBlock);
}

TEST_F(HogDBTest, InvalidatedBlockTXLockReturnsStaleBlock)
{
  constexpr uint64_t kInvalidAddr = 0xABCD0003;
  Address addr = make_address(kInvalidAddr);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  block_tx.get().invalidate();

  ItemKey key = make_item_key(addr, 0);
  ItemChange change = ItemChange(NNInt128(0, 10));
  auto result = block_tx.get().lock(key, change);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), HogDBCode::kStaleBlock);
}

TEST_F(HogDBTest, ViewingBlockTXLockRejectsWrite)
{
  constexpr uint64_t kViewingAddr = 0xABCD0010;
  Address addr = make_address(kViewingAddr);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeViewing);

  ItemKey key = make_item_key(addr, 0);
  ItemChange change = ItemChange(NNInt128(0, 10));
  auto result = block_tx.get().lock(key, change);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), HogDBCode::kInvalidExecutorBehavior);
}

TEST_F(HogDBTest, ViewingBlockTXCommitRejectsWrite)
{
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeViewing);

  auto result = block_tx.get().commit();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), HogDBCode::kInvalidExecutorBehavior);
}

// Regression test for issue #1750: a stale sibling block could read the live
// DAO's storage cost after another sibling committed, inflating refunds.
// The staleness guard (PR #1895) must reject the stale block's lock attempt.
TEST_F(HogDBTest, StaleSiblingBytestringLockRejectsAfterSiblingCommit)
{
  Address addr = make_address(kAddress + 301);
  ItemKey j_key = make_item_key(addr, 1);
  ItemKey k_key = make_item_key(addr, 2);

  const int128_t initial_balance = 10'000'000;
  ASSERT_TRUE(db_.write_address_balance(addr, initial_balance));
  ASSERT_TRUE(db_.write_address_metadata(
      addr,
      DeployedContractHash{.hash = sha256(std::as_bytes(std::span("stale"))),
                           .contract_size = 10}));

  PerThreadARC<BlockTX> block_a(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  PerThreadARC<BlockTX> block_b(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  ASSERT_TRUE(block_a.try_acquire_reference());
  {
    InvokeTX itx_a(block_a);
    ASSERT_TRUE(
        itx_a.request_nnint128(j_key, 1,
                               std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(itx_a.sync_balance_locks());
    ASSERT_TRUE(itx_a.commit());
    itx_a.release_blocktx_reference();
  }

  std::array<std::byte, 1024> large_value;
  large_value.fill(std::byte{0x42});
  ASSERT_TRUE(block_b.try_acquire_reference());
  {
    InvokeTX itx_b(block_b);
    ASSERT_TRUE(
        itx_b.request_bytes(k_key, std::span(large_value),
                            std::to_underlying(StorageDeltaFlags::None)));
    ASSERT_TRUE(itx_b.sync_balance_locks());
    ASSERT_TRUE(itx_b.commit());
    itx_b.release_blocktx_reference();
  }
  ASSERT_TRUE(block_b.get().close_skip_hash_for_testing());
  ASSERT_TRUE(block_b.get().commit());

  // Simulate what RPCContext::invalidate_siblings does when B commits.
  block_a.get().invalidate();

  // Block A is now stale — sibling B committed, advancing the tip.
  // The staleness guard must reject A's attempt to lock key K.
  // Without the guard, lock() would read B's 1024-byte cost from the live DAO
  // as A's snapshot cost, producing a spurious storage refund.
  ASSERT_TRUE(block_a.try_acquire_reference());
  {
    InvokeTX itx_a(block_a);
    constexpr std::array<std::byte, 1> kSmallValue = {std::byte{0x7F}};
    auto result =
        itx_a.request_bytes(k_key, std::span(kSmallValue),
                            std::to_underlying(StorageDeltaFlags::None));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), HogDBCode::kStaleBlock);

    itx_a.abort();
    itx_a.release_blocktx_reference();
  }
}

TEST_F(HogDBTest, ExclusiveRCQCommitStaleBlock)
{
  constexpr auto kNoFlags = std::to_underlying(StorageDeltaFlags::None);
  constexpr auto kExclusiveFlags =
      std::to_underlying(StorageDeltaFlags::ExclusiveLock);

  Address trigger_addr = make_address(kAddress + 18);
  Address control_addr = make_address(kAddress + 19);
  ItemKey trigger_key = make_key(kAddress + 18, 1);
  ItemKey control_key = make_key(kAddress + 19, 1);
  setup_contract_address(trigger_addr);
  setup_contract_address(control_addr);

  auto make_insert_delta = [](uint8_t byte) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{byte});
    return delta;
  };

  DbHash parent_hash = commit_block_and_return_db_hash(1, [&](InvokeTX &itx) {
    auto trigger_seed = make_insert_delta(0x11);
    ASSERT_TRUE(
        itx.request_retrocausal_queue(trigger_key, trigger_seed, kNoFlags));
    auto control_seed = make_insert_delta(0x22);
    ASSERT_TRUE(
        itx.request_retrocausal_queue(control_key, control_seed, kNoFlags));
  });
  ASSERT_TRUE(db_.read_retrocausal_queue_metadata(trigger_key));
  ASSERT_TRUE(db_.read_retrocausal_queue_metadata(control_key));

  PerThreadARC<BlockTX> stale_block_tx(
      db_, cost_model_, background_commit_worker_, 2,
      BlockExecutionMode::kBlockExecutionModeProposal);

  ASSERT_TRUE(stale_block_tx.try_acquire_reference());
  InvokeTX insert_only_tx(stale_block_tx);
  ASSERT_TRUE(insert_only_tx.request_deletion(control_key, kExclusiveFlags));
  auto control_delta = make_insert_delta(0x33);
  ASSERT_TRUE(insert_only_tx.request_retrocausal_queue(control_key,
                                                       control_delta,
                                                       kNoFlags));
  ASSERT_TRUE(insert_only_tx.sync_balance_locks());

  ASSERT_TRUE(stale_block_tx.try_acquire_reference());
  InvokeTX consuming_tx(stale_block_tx);
  ASSERT_TRUE(consuming_tx.request_deletion(trigger_key, kExclusiveFlags));
  auto consuming_delta = make_insert_delta(0x44);
  consuming_delta.consume.emplace();
  consuming_delta.consume->past_block_consume.max_fully_consumed_block_number =
      1;
  consuming_delta.consume->past_block_consume.partial_consumed_number = 0;
  ASSERT_TRUE(consuming_tx.request_retrocausal_queue(trigger_key,
                                                     consuming_delta,
                                                     kNoFlags));
  ASSERT_TRUE(consuming_tx.sync_balance_locks());

  PerThreadARC<BlockTX> sibling_block_tx(
      db_, cost_model_, background_commit_worker_, 2,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(sibling_block_tx.try_acquire_reference());
  {
    InvokeTX sibling_tx(sibling_block_tx);
    ASSERT_TRUE(sibling_tx.request_deletion(trigger_key, kNoFlags));
    ASSERT_TRUE(sibling_tx.request_deletion(control_key, kNoFlags));
    ASSERT_TRUE(sibling_tx.sync_balance_locks());
    ASSERT_TRUE(sibling_tx.commit());
    sibling_tx.release_blocktx_reference();
  }

  ASSERT_TRUE(sibling_block_tx.get().close(parent_hash));
  stale_block_tx.get().invalidate();
  ASSERT_TRUE(sibling_block_tx.get().commit());
  ASSERT_FALSE(db_.read_retrocausal_queue_metadata(trigger_key));
  ASSERT_FALSE(db_.read_retrocausal_queue_metadata(control_key));

  // Insert-only exclusive RCQ commit succeeds even on a stale block because
  // the re-validation has no past-block consume to check against the DAO.
  ASSERT_TRUE(insert_only_tx.commit());
  insert_only_tx.release_blocktx_reference();

  // Consuming exclusive RCQ commit fails: the re-validation reads deleted
  // RCQ blocks from the live DAO.
  auto r = consuming_tx.commit();
  ASSERT_FALSE(r.has_value());
  consuming_tx.release_blocktx_reference();

  // The failed commit poisons the BlockTX.
  ASSERT_TRUE(stale_block_tx.get().is_poisoned());

  // A poisoned BlockTX refuses to close or commit.
  ASSERT_FALSE(stale_block_tx.get().close(parent_hash).has_value());
}

TEST_F(HogDBTest, ExclusiveRCQCommitNonStaleBlockSucceeds)
{
  constexpr auto kNoFlags = std::to_underlying(StorageDeltaFlags::None);
  constexpr auto kExclusiveFlags =
      std::to_underlying(StorageDeltaFlags::ExclusiveLock);

  Address addr = make_address(kAddress + 20);
  ItemKey key = make_key(kAddress + 20, 1);
  setup_contract_address(addr);

  auto make_insert_delta = [](uint8_t byte) {
    RetrocausalQueueDelta delta;
    delta.insert.added_entries.push_back(QueueEntry{byte});
    return delta;
  };

  DbHash parent_hash = commit_block_and_return_db_hash(1, [&](InvokeTX &itx) {
    auto seed = make_insert_delta(0x11);
    ASSERT_TRUE(itx.request_retrocausal_queue(key, seed, kNoFlags));
  });
  ASSERT_TRUE(db_.read_retrocausal_queue_metadata(key));

  // Exclusive RCQ delete + recreate with consume on a non-stale block.
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 2,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX itx(block_tx);
  ASSERT_TRUE(itx.request_deletion(key, kExclusiveFlags));
  auto consuming_delta = make_insert_delta(0x22);
  consuming_delta.consume.emplace();
  consuming_delta.consume->past_block_consume.max_fully_consumed_block_number =
      1;
  consuming_delta.consume->past_block_consume.partial_consumed_number = 0;
  ASSERT_TRUE(itx.request_retrocausal_queue(key, consuming_delta, kNoFlags));
  ASSERT_TRUE(itx.sync_balance_locks());

  // Commit succeeds on a non-stale block.
  ASSERT_TRUE(itx.commit());
  itx.release_blocktx_reference();

  // Block is not poisoned, close and commit succeed.
  ASSERT_FALSE(block_tx.get().is_poisoned());
  ASSERT_TRUE(block_tx.get().close(parent_hash));
  ASSERT_TRUE(block_tx.get().commit());
}

// Regression: rollback_to_savepoint must not crash or leave inconsistent state
// when restoring an RCQ consume whose past block was pruned by a committed
// sibling block. modify_lock re-derives the consume's past-block cost from the
// live DAO; a sibling that deletes the queue and commits removes that block
// (and invalidates this block, as invalidate_siblings would). The restore's
// re-read then returns kNotFound. rollback_to_savepoint must surface that as a
// stale block rather than assert(ret)-crash / desync the lock from changes_.
TEST_F(HogDBTest, RollbackToSavepointStaleRCQConsumeReturnsStaleBlock)
{
  constexpr auto kNoFlags = std::to_underlying(StorageDeltaFlags::None);

  Address queue_addr = make_address(kAddress + 31);
  Address other_addr = make_address(kAddress + 32);
  ItemKey queue_key = make_key(kAddress + 31, 1);
  ItemKey other_key = make_key(kAddress + 32, 1);
  setup_contract_address(queue_addr);
  setup_contract_address(other_addr);

  // A delta that consumes `partial` entries from past block 1 and inserts
  // `inserts`. The past-block consume forces try_atomic_change_lock to read
  // block 1's size from the DAO.
  auto make_consume_insert_delta = [](uint64_t partial,
                                      const std::vector<uint8_t> &inserts) {
    RetrocausalQueueDelta delta;
    for (uint8_t b : inserts) {
      delta.insert.added_entries.push_back(QueueEntry{b});
    }
    delta.consume.emplace();
    delta.consume->past_block_consume.max_fully_consumed_block_number = 0;
    delta.consume->past_block_consume.partial_consumed_number = partial;
    return delta;
  };

  // Block 1: seed the queue with one entry, producing block 1 in the DAO.
  DbHash parent_hash = commit_block_and_return_db_hash(1, [&](InvokeTX &itx) {
    RetrocausalQueueDelta seed;
    seed.insert.added_entries.push_back(QueueEntry{uint8_t{0x11}});
    ASSERT_TRUE(itx.request_retrocausal_queue(queue_key, seed, kNoFlags));
  });
  ASSERT_TRUE(db_.read_retrocausal_queue_block(queue_key, 1));

  // Block 2 (the to-be-stale block): a consume that reads past block 1, then a
  // savepoint and a second consume that grows the insert high-water mark so the
  // restore takes the shrink branch.
  PerThreadARC<BlockTX> stale_block_tx(
      db_, cost_model_, background_commit_worker_, 2,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(stale_block_tx.try_acquire_reference());
  InvokeTX stale_tx(stale_block_tx);

  auto depth0 = make_consume_insert_delta(/*partial=*/1, {0xAA});
  ASSERT_TRUE(stale_tx.request_retrocausal_queue(queue_key, depth0, kNoFlags));

  int savepoint = stale_tx.create_savepoint();
  auto depth1 = make_consume_insert_delta(/*partial=*/1, {0xAA, 0xBB});
  ASSERT_TRUE(stale_tx.request_retrocausal_queue(queue_key, depth1, kNoFlags));

  // A sibling block-2 deletes the same queue and commits, pruning block 1 from
  // the live DAO and invalidating the stale block (as invalidate_siblings
  // would during commit publication).
  PerThreadARC<BlockTX> sibling_block_tx(
      db_, cost_model_, background_commit_worker_, 2,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(sibling_block_tx.try_acquire_reference());
  {
    InvokeTX sibling_tx(sibling_block_tx);
    ASSERT_TRUE(sibling_tx.request_deletion(queue_key, kNoFlags));
    ASSERT_TRUE(sibling_tx.sync_balance_locks());
    ASSERT_TRUE(sibling_tx.commit());
    sibling_tx.release_blocktx_reference();
  }
  ASSERT_TRUE(sibling_block_tx.get().close(parent_hash));
  stale_block_tx.get().invalidate();
  ASSERT_TRUE(sibling_block_tx.get().commit());
  ASSERT_FALSE(db_.read_retrocausal_queue_block(queue_key, 1));

  // Roll back the savepoint. The restore re-derives the depth-0 consume cost by
  // reading the now-deleted block 1. Before the fix this asserted(ret) and
  // crashed; now it must return cleanly.
  stale_tx.rollback_to_savepoint(savepoint - 1);

  // The block stays invalidated and the caller observes kStaleBlock on its next
  // locked operation.
  EXPECT_TRUE(stale_block_tx.get().is_invalidated());
  std::array<std::byte, 1> follow_up_buf{std::byte{0x01}};
  auto follow_up = stale_tx.request_bytes(other_key, follow_up_buf, kNoFlags);
  ASSERT_FALSE(follow_up.has_value());
  EXPECT_EQ(follow_up.error().code(), HogDBCode::kStaleBlock);

  stale_tx.release_blocktx_reference();
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
