#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "hogd/base/types.h"
#include "hogd/hog_auth/auth_data_cache.h"
#include "hogd/init.h"
#include "types.hh"

namespace hogd {

namespace {

constexpr ContractAddress
make_test_address(uint8_t id)
{
  ContractAddress address{};
  address[0] = id;
  return address;
}

} // namespace

TEST(AuthDataCacheTest, BasicPutAndGet)
{
  AuthDataCache cache(4);

  ContractAddress addr1 = make_test_address(1);
  BlockHeight height1 = 100;
  AddressAuthData auth_data1{};
  auto data1 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height1, auth_data1);

  // Put data into cache
  cache.put(addr1, data1);

  // Retrieve it
  auto retrieved = cache.get(addr1);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->first, height1);
}

TEST(AuthDataCacheTest, GetNonExistentAddress)
{
  AuthDataCache cache(4);

  ContractAddress addr1 = make_test_address(1);
  ContractAddress addr2 = make_test_address(2);

  BlockHeight height1 = 100;
  AddressAuthData auth_data1{};
  auto data1 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height1, auth_data1);

  cache.put(addr1, data1);

  // Try to get a different address
  auto retrieved = cache.get(addr2);
  EXPECT_EQ(retrieved, nullptr);
}

TEST(AuthDataCacheTest, UpdateWithNewerHeight)
{
  AuthDataCache cache(4);

  ContractAddress addr1 = make_test_address(1);
  BlockHeight height1 = 100;
  BlockHeight height2 = 200;
  AddressAuthData auth_data1{};
  AddressAuthData auth_data2{};

  auto data1 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height1, auth_data1);
  auto data2 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height2, auth_data2);

  cache.put(addr1, data1);
  cache.put(addr1, data2);

  // Should get the newer data
  auto retrieved = cache.get(addr1);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->first, height2);
}

TEST(AuthDataCacheTest, IgnoreOlderUpdate)
{
  AuthDataCache cache(4);

  ContractAddress addr1 = make_test_address(1);
  BlockHeight height1 = 200;
  BlockHeight height2 = 100; // Older than height1
  AddressAuthData auth_data1{};
  AddressAuthData auth_data2{};

  auto data1 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height1, auth_data1);
  auto data2 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height2, auth_data2);

  cache.put(addr1, data1);
  cache.put(addr1, data2); // This should be ignored

  // Should still have the newer data
  auto retrieved = cache.get(addr1);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->first, height1);
}

TEST(AuthDataCacheTest, MultipleAddresses)
{
  AuthDataCache cache(4);

  ContractAddress addr1 = make_test_address(1);
  ContractAddress addr2 = make_test_address(2);
  ContractAddress addr3 = make_test_address(3);

  BlockHeight height1 = 100;
  BlockHeight height2 = 200;
  BlockHeight height3 = 300;

  AddressAuthData auth_data1{};
  AddressAuthData auth_data2{};
  AddressAuthData auth_data3{};

  auto data1 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height1, auth_data1);
  auto data2 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height2, auth_data2);
  auto data3 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height3, auth_data3);

  cache.put(addr1, data1);
  cache.put(addr2, data2);
  cache.put(addr3, data3);

  // Verify all can be retrieved
  auto retrieved1 = cache.get(addr1);
  ASSERT_NE(retrieved1, nullptr);
  EXPECT_EQ(retrieved1->first, height1);

  auto retrieved2 = cache.get(addr2);
  ASSERT_NE(retrieved2, nullptr);
  EXPECT_EQ(retrieved2->first, height2);

  auto retrieved3 = cache.get(addr3);
  ASSERT_NE(retrieved3, nullptr);
  EXPECT_EQ(retrieved3->first, height3);
}

TEST(AuthDataCacheTest, Eviction)
{
  detail::AuthDataCacheBucket bucket;
  std::vector<ContractAddress> addresses;
  addresses.reserve(100);
  for (int i = 0; i < 100; i++) {
    addresses.push_back(make_test_address(i));
  }
  for (const auto &addr : addresses) {
    bucket.put(addr,
               std::make_shared<
                   std::pair<BlockHeight, std::optional<AddressAuthData>>>(
                   0, std::nullopt));
  }
  for (size_t i = 0; i < 100 - 16; i++) {
    ASSERT_EQ(bucket.get(addresses[i]), nullptr);
  }
  for (size_t i = 100 - 16; i < 100; i++) {
    ASSERT_NE(bucket.get(addresses[i]), nullptr);
  }
}

TEST(AuthDataCacheTest, RejectOlderUpdateAfterEviction)
{
  AuthDataCache cache(1); // Single bucket for predictable behavior

  ContractAddress addr1 = make_test_address(1);
  ContractAddress addr2 = make_test_address(2);
  AddressAuthData auth_data{};

  // Insert at height 10
  auto data10 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          10, auth_data);
  cache.put(addr1, data10);

  // Verify it's in the cache
  auto retrieved = cache.get(addr1);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->first, 10);

  // Evict everything
  cache.evict_all();

  // Verify it's been evicted
  retrieved = cache.get(addr1);
  ASSERT_EQ(retrieved, nullptr);

  // Try to reinsert at height 9 (older than height 10)
  auto data9 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          9, auth_data);
  cache.put(addr1, data9);

  // The update should be rejected because height 9 < max_observed_height (10)
  // So the address should still not be in the cache
  retrieved = cache.get(addr1);
  ASSERT_EQ(retrieved, nullptr);

  // an update for a different address at height 9 should be rejected
  auto data9_2 =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          9, auth_data);
  cache.put(addr2, data9_2);
  retrieved = cache.get(addr2);
  ASSERT_EQ(retrieved, nullptr);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
