#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>

// xdr types
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
  SHA256Hash hash = sha256(std::as_bytes(std::span(test_code)));
  EXPECT_TRUE(db_.write_address_wasm_hash(addr, hash));
  HogDBStatus<SHA256Hash> rhash = db_.read_address_wasm_hash(addr);
  EXPECT_TRUE(rhash);
  if (rhash) {
    EXPECT_EQ(hash, *rhash);
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
  HogDBStatus<size_t> rsize = db_.get_item_size(ikey);
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
  rsize = db_.get_item_size(ikey);
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
  EXPECT_EQ(ItemChange(NNInt128(10, 1)).merge(ItemChange(NNInt128(20, 1))),
            HogDBError(HogDBCode::kConflict));

  // try using nnint128 as a lock (conflict)
  EXPECT_EQ(ItemChange(NNInt128(1, -1)).merge(ItemChange(NNInt128(1, -1))),
            HogDBError(HogDBCode::kConflict));

  // try positive addition (no conflict)
  EXPECT_TRUE(ItemChange(NNInt128(0, 10)).merge(ItemChange(NNInt128(0, 10))));

  // try subtract to zero (no conflict)
  EXPECT_TRUE(ItemChange(NNInt128(10, -5)).merge(ItemChange(NNInt128(10, -5))));

  // try subtract below zero (conflict)
  EXPECT_EQ(ItemChange(NNInt128(10, -6)).merge(ItemChange(NNInt128(10, -5))),
            HogDBError(HogDBCode::kConflict));

  //
  // Test Byte changes
  //

  std::string a("a");
  std::string b("b");
  auto as = std::as_bytes(std::span(a));
  auto bs = std::as_bytes(std::span(b));

  // try same bytes (no conflict)
  EXPECT_TRUE(ItemChange(Bytes(as)).merge(ItemChange(Bytes(as))));

  // try different bytes (conflict)
  EXPECT_EQ(ItemChange(Bytes(as)).merge(ItemChange(Bytes(bs))),
            HogDBError(HogDBCode::kConflict));
}

TEST_F(HogDBTest, StorageCost)
{
  ItemKey ikey = make_key(kAddress, kAddress);

  // test new item creation
  {
    ItemChange change(NNInt128(1, 1));
    HogDBStatus<ssize_t> delta = change.storage_cost_delta(db_, ikey);
    ASSERT_TRUE(delta);
    if (delta) {
      EXPECT_GT(*delta, 0);
    }
  }

  // test modification to existing item
  int128_t test_nnint128 = 10;
  EXPECT_TRUE(db_.write_nnint128(ikey, test_nnint128));
  {
    ItemChange change(NNInt128(1, 1));
    HogDBStatus<ssize_t> delta = change.storage_cost_delta(db_, ikey);
    ASSERT_TRUE(delta);
    if (delta) {
      EXPECT_EQ(*delta, 0);
    }
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
