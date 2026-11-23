#include <gtest/gtest.h>

#include <unordered_map>

#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/item/frame_item_key.h"
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
addrkey_of(uint8_t seed, StorageKeyType type)
{
  StorageKey key(type);
  if (type == StorageKeyType::ContractData) {
    key.contract_data_key() = data_key_of(seed);
  }
  return AddressAndKey{.address = address_of(seed), .key = key};
}

// Every key type survives the trip through the flat form and back, which is
// what a delta emitted from the frame map depends on.
TEST(FrameItemKeyTest, RoundTripsEveryKeyType)
{
  for (const auto type :
       {StorageKeyType::ContractData, StorageKeyType::FreeNativeTokenBalance,
        StorageKeyType::AuthData, StorageKeyType::Metadata,
        StorageKeyType::ReplayCache}) {
    const AddressAndKey original = addrkey_of(7, type);
    const AddressAndKey back =
        FrameItemKey::from(original).to_address_and_key();
    EXPECT_EQ(back, original) << static_cast<int>(type);
  }
}

// The syscall path builds the key from its parts; it must be the same key the
// XDR path builds, or a write staged one way is invisible to a read the other.
TEST(FrameItemKeyTest, ContractDataMatchesTheXdrForm)
{
  const AddressAndKey via_xdr = addrkey_of(3, StorageKeyType::ContractData);
  const FrameItemKey direct =
      FrameItemKey::contract_data(address_of(3), data_key_of(3));
  EXPECT_EQ(direct, FrameItemKey::from(via_xdr));
  EXPECT_EQ(direct.hash(), FrameItemKey::from(via_xdr).hash());
}

TEST(FrameItemKeyTest, DistinguishesAddressTypeAndPayload)
{
  const FrameItemKey base =
      FrameItemKey::from(addrkey_of(1, StorageKeyType::ContractData));
  EXPECT_NE(base,
            FrameItemKey::from(addrkey_of(2, StorageKeyType::ContractData)));
  EXPECT_NE(base, FrameItemKey::from(addrkey_of(1, StorageKeyType::AuthData)));
  EXPECT_NE(base, FrameItemKey::contract_data(address_of(1), data_key_of(9)));
  // Two payload-less types under one address are different keys.
  EXPECT_NE(FrameItemKey::from(addrkey_of(1, StorageKeyType::AuthData)),
            FrameItemKey::from(addrkey_of(1, StorageKeyType::Metadata)));
}

// The hasher returns the cached hash, so a map keyed this way never re-hashes
// the bytes -- the whole point of the type.
TEST(FrameItemKeyTest, MapUsesTheCachedHash)
{
  std::unordered_map<FrameItemKey, int, FrameItemKeyHash> map;
  const FrameItemKey key =
      FrameItemKey::from(addrkey_of(5, StorageKeyType::ContractData));
  EXPECT_EQ(FrameItemKeyHash{}(key), key.hash());
  map.emplace(key, 42);
  const auto it =
      map.find(FrameItemKey::contract_data(address_of(5), data_key_of(5)));
  ASSERT_NE(it, map.end());
  EXPECT_EQ(it->second, 42);
}

} // namespace
} // namespace hogd
