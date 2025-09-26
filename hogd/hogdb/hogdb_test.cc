#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>

// xdr types
#include "account.hh"
#include "db_coord.hh"
#include "types.hh"

#include "hogd/base/io.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
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
  HogDBStatus<size_t> rsize = db_.get_charged_storage_cost(ikey);
  EXPECT_TRUE(rsize);
  if (rsize) {
    EXPECT_EQ(sizeof(test_nnint128), *rsize);
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
  rsize = db_.get_charged_storage_cost(ikey);
  EXPECT_TRUE(rsize);
  if (rsize) {
    EXPECT_EQ(test_bytes.size(), *rsize);
  }
  rtype = db_.get_item_type(ikey);
  EXPECT_TRUE(rtype);
  if (rtype) {
    EXPECT_EQ(kBytes, *rtype);
  }
  EXPECT_TRUE(db_.remove_item(ikey));
}

TEST_F(HogDBTest, ItemChange)
{
  //
  // Test NNInt128 changes
  //

  // try different bases (conflict)
  {
    auto x = ItemChange(NNInt128(10, 1));
    x.set_snapshot_cost(0);
    EXPECT_EQ(x.merge(ItemChange(NNInt128(20, 1))),
              HogDBStatus<int128_t>(HogDBError(HogDBCode::kConflict)));
  }

  // try using nnint128 as a lock (conflict)
  {
    auto x = ItemChange(NNInt128(1, -1));
    x.set_snapshot_cost(0);
    EXPECT_EQ(x.merge(ItemChange(NNInt128(1, -1))),
              HogDBStatus<int128_t>(HogDBError(HogDBCode::kConflict)));
  }

  // try positive addition (no conflict)
  {
    auto x = ItemChange(NNInt128(0, 10));
    x.set_snapshot_cost(0);
    EXPECT_TRUE(x.merge(ItemChange(NNInt128(0, 5))));
  }

  // try subtract to zero (no conflict)
  {
    auto x = ItemChange(NNInt128(10, -5));
    x.set_snapshot_cost(0);
    EXPECT_TRUE(x.merge(ItemChange(NNInt128(10, -5))));
  }

  // try subtract below zero (conflict)
  {
    auto x = ItemChange(NNInt128(10, -6));
    x.set_snapshot_cost(0);
    EXPECT_EQ(x.merge(ItemChange(NNInt128(10, -5))),
              HogDBStatus<int128_t>(HogDBError(HogDBCode::kConflict)));
  }

  //
  // Test Byte changes
  //

  std::string a("a");
  std::string b("b");
  auto as = std::as_bytes(std::span(a));
  auto bs = std::as_bytes(std::span(b));

  // try same bytes (no conflict)
  {
    auto x = ItemChange(Bytes(as));
    x.set_snapshot_cost(0);
    EXPECT_TRUE(x.merge(ItemChange(Bytes(as))));
  }

  // try different bytes (conflict)
  {
    auto x = ItemChange(Bytes(as));
    x.set_snapshot_cost(0);
    EXPECT_EQ(x.merge(ItemChange(Bytes(bs))),
              HogDBStatus<int128_t>(HogDBError(HogDBCode::kConflict)));
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
