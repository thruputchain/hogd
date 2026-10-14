#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/frame_item_key.h"
#include "hogd/hog_exec/runtime/item/item_view_type.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/test_util/block_snapshot_server.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {
namespace {

ContractAddress
address_of(uint8_t seed)
{
  ContractAddress address{};
  for (size_t i = 0; i < address.size(); ++i) {
    address[i] = static_cast<uint8_t>(seed + i);
  }
  return address;
}

StorageKeyContractData
data_key_of(uint8_t seed)
{
  StorageKeyContractData key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>((seed * 3) + i);
  }
  return key;
}

AddressAndKey
contract_data_addrkey(uint8_t seed)
{
  StorageKey key(StorageKeyType::ContractData);
  key.contract_data_key() = data_key_of(seed);
  return AddressAndKey{.address = address_of(seed), .key = key};
}

Bytestring
make_bytestring(std::string_view str)
{
  Bytestring bytestring;
  bytestring.resize_uninitialized(str.size());
  std::memcpy(bytestring.data(), str.data(), str.size());
  return bytestring;
}

// The cache is keyed by FrameItemKey. The syscall path probes it with the key
// it built for the frame maps; tests and tooling probe it with the XDR
// AddressAndKey. Both must land on the same entry, or a first touch through
// one path would miss what the other loaded.
class StorageSnapshotCacheTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    server_.start_server(/*port=*/0);
    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = server_.port()});
    cache_ = std::make_unique<StorageSnapshotCache>(
        BlockID{1},
        BlockExecutionContext{.block_height = 1, .block_timepoint = 0},
        *db_proxy_);
    auto res = cache_->load_db_tip()();
    if (!res.has_value()) [[unlikely]] {
      panic("StorageSnapshotCacheTest::SetUp: failed to load db tip: {}",
            res.error());
    }
  }

  void seed_bytestring(AddressAndKey const &addrkey, std::string_view text)
  {
    server_->impl()->setup_create_item(addrkey,
                                       StorageItemWrapper(make_bytestring(text),
                                                          &cost_model_));
  }

  BlockSnapshotServer server_ = BlockSnapshotServer(/*block_id=*/1);
  StorageCostModel cost_model_{12000, 1, 5, 5, 1234};
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<StorageSnapshotCache> cache_;
};

TEST_F(StorageSnapshotCacheTest, FrameKeyAndXdrKeyShareOneEntry)
{
  const AddressAndKey addrkey = contract_data_addrkey(3);
  seed_bytestring(addrkey, "value-3");

  // The syscall path's key: built from the address and the 32-byte key with
  // no XDR union in between. This lookup fills the cache.
  auto via_frame = cache_->try_get_snapshot_item(
      FrameItemKey::contract_data(address_of(3), data_key_of(3)))();
  ASSERT_TRUE(via_frame.has_value());
  ASSERT_NE(via_frame.value(), nullptr);
  EXPECT_EQ(via_frame.value()->type(), ItemViewType::kBytestring);
  EXPECT_EQ(via_frame.value()->bytestring(), make_bytestring("value-3"));

  // The XDR path finds the same entry, not a second copy.
  auto via_xdr = cache_->try_get_snapshot_item(addrkey)();
  ASSERT_TRUE(via_xdr.has_value());
  EXPECT_EQ(via_xdr.value(), via_frame.value());
}

TEST_F(StorageSnapshotCacheTest, NegativeLookupIsCachedUnderTheFrameKey)
{
  const AddressAndKey addrkey = contract_data_addrkey(4);

  auto via_xdr = cache_->try_get_snapshot_item(addrkey)();
  ASSERT_TRUE(via_xdr.has_value());
  ASSERT_NE(via_xdr.value(), nullptr);
  EXPECT_EQ(via_xdr.value()->type(), std::nullopt);

  auto via_frame = cache_->try_get_snapshot_item(
      FrameItemKey::contract_data(address_of(4), data_key_of(4)))();
  ASSERT_TRUE(via_frame.has_value());
  EXPECT_EQ(via_frame.value(), via_xdr.value());
}

// Two payload-less key types under one address are different entries: the
// key type is part of the flat key, so the cache cannot conflate them. (Both
// types here are ones the cache serves; Metadata is answered by the
// deployment cache instead and make_variant rejects it.)
TEST_F(StorageSnapshotCacheTest, PayloadlessKeyTypesAreDistinctEntries)
{
  const AddressAndKey auth{.address = address_of(5),
                           .key = StorageKey(StorageKeyType::AuthData)};
  const AddressAndKey meta{.address = address_of(5),
                           .key = StorageKey(
                               StorageKeyType::FreeNativeTokenBalance)};

  auto auth_item = cache_->try_get_snapshot_item(auth)();
  auto meta_item = cache_->try_get_snapshot_item(meta)();
  ASSERT_TRUE(auth_item.has_value());
  ASSERT_TRUE(meta_item.has_value());
  ASSERT_NE(auth_item.value(), nullptr);
  ASSERT_NE(meta_item.value(), nullptr);
  EXPECT_NE(auth_item.value(), meta_item.value());

  // And each is found again by its frame key.
  EXPECT_EQ(cache_->try_get_snapshot_item(FrameItemKey::from(auth))().value(),
            auth_item.value());
  EXPECT_EQ(cache_->try_get_snapshot_item(FrameItemKey::from(meta))().value(),
            meta_item.value());
}

} // namespace
} // namespace hogd
