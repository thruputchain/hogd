#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "hogd/base/types.h"
#include "hogd/indexer/key_format.h"
#include "types.hh"

namespace hogd {

class IndexerKeyFormatTest : public ::testing::Test {
protected:
  void SetUp() override {}
};

// Test transaction key format
TEST_F(IndexerKeyFormatTest, TxKeyByteOrder)
{
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0xAA);

  std::string key;
  IndexerKeyFormat::make_tx_key(key, tx_hash);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kTxKeySize);

  // Verify tx_hash
  EXPECT_EQ(static_cast<uint8_t>(key[0]), 0xAA);
  EXPECT_EQ(static_cast<uint8_t>(key[31]), 0xAA);
}

// Test contract key prefix format - verify block_height is big-endian
TEST_F(IndexerKeyFormatTest, ContractKeyPrefixByteOrder)
{
  ContractAddress address;
  std::ranges::fill(address, 0x11);
  BlockHeight block_height = 0x0123456789ABCDEF;

  std::string key;
  IndexerKeyFormat::make_contract_key_prefix(
      key, address, IndexerKeyFormat::kEvent, block_height);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kContractKeyPrefixSize);

  // Verify contract address is first (32 bytes)
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0x11);
  }

  // Verify the kind byte immediately follows the address
  EXPECT_EQ(static_cast<uint8_t>(key[32]), IndexerKeyFormat::kEvent);

  // Verify block_height is stored in big-endian order after the kind byte
  EXPECT_EQ(static_cast<uint8_t>(key[33]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(key[34]), 0x23);
  EXPECT_EQ(static_cast<uint8_t>(key[35]), 0x45);
  EXPECT_EQ(static_cast<uint8_t>(key[36]), 0x67);
  EXPECT_EQ(static_cast<uint8_t>(key[37]), 0x89);
  EXPECT_EQ(static_cast<uint8_t>(key[38]), 0xAB);
  EXPECT_EQ(static_cast<uint8_t>(key[39]), 0xCD);
  EXPECT_EQ(static_cast<uint8_t>(key[40]), 0xEF);
}

// Test contract key format - verify block_height and sequence_number are
// big-endian
TEST_F(IndexerKeyFormatTest, ContractKeyByteOrder)
{
  ContractAddress address;
  std::ranges::fill(address, 0x22);
  BlockHeight block_height = 0x0123456789ABCDEF;
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x33);
  uint32_t sequence_number = 0x12345678;

  std::string key;
  IndexerKeyFormat::make_contract_key(key, address, IndexerKeyFormat::kEvent,
                                      block_height, tx_hash, sequence_number);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kContractKeySize);

  // Verify contract address is first (32 bytes)
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0x22);
  }

  // Verify the kind byte immediately follows the address
  EXPECT_EQ(static_cast<uint8_t>(key[32]), IndexerKeyFormat::kEvent);

  // Verify block_height is big-endian (bytes 33-40)
  EXPECT_EQ(static_cast<uint8_t>(key[33]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(key[34]), 0x23);
  EXPECT_EQ(static_cast<uint8_t>(key[35]), 0x45);
  EXPECT_EQ(static_cast<uint8_t>(key[36]), 0x67);
  EXPECT_EQ(static_cast<uint8_t>(key[37]), 0x89);
  EXPECT_EQ(static_cast<uint8_t>(key[38]), 0xAB);
  EXPECT_EQ(static_cast<uint8_t>(key[39]), 0xCD);
  EXPECT_EQ(static_cast<uint8_t>(key[40]), 0xEF);

  // Verify tx_hash follows (bytes 41-72)
  for (size_t i = 41; i < 73; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0x33);
  }

  // Verify sequence_number is big-endian (bytes 73-76)
  EXPECT_EQ(static_cast<uint8_t>(key[73]), 0x12);
  EXPECT_EQ(static_cast<uint8_t>(key[74]), 0x34);
  EXPECT_EQ(static_cast<uint8_t>(key[75]), 0x56);
  EXPECT_EQ(static_cast<uint8_t>(key[76]), 0x78);
}

// Test contract key parsing round-trip
TEST_F(IndexerKeyFormatTest, ContractKeyRoundTrip)
{
  ContractAddress original_address;
  std::ranges::fill(original_address, 0x44);
  BlockHeight original_height = 0x1122334455667788;
  SHA256Hash original_tx_hash;
  std::ranges::fill(original_tx_hash, 0x55);
  uint32_t original_seq = 0xAABBCCDD;

  std::string key;
  IndexerKeyFormat::make_contract_key(key, original_address,
                                      IndexerKeyFormat::kEvent, original_height,
                                      original_tx_hash, original_seq);

  auto parsed = IndexerKeyFormat::parse_contract_key(key);
  ASSERT_TRUE(parsed.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT above
  const auto &p = parsed.value();
  EXPECT_EQ(p.address, original_address);
  EXPECT_EQ(p.kind, IndexerKeyFormat::kEvent);
  EXPECT_EQ(p.block_height, original_height);
  EXPECT_EQ(p.tx_hash, original_tx_hash);
  EXPECT_EQ(p.sequence_number, original_seq);
}

// Test contract key parsing with wrong size
TEST_F(IndexerKeyFormatTest, ContractKeyParseWrongSize)
{
  std::string wrong_key = "too short";

  auto parsed = IndexerKeyFormat::parse_contract_key(wrong_key);

  EXPECT_FALSE(parsed.has_value());
}

// Test that contract key prefixes are lexicographically ordered by block_height
TEST_F(IndexerKeyFormatTest, ContractKeyPrefixLexicographicOrder)
{
  ContractAddress address;
  std::ranges::fill(address, 0x99);

  std::string key1, key2, key3;
  IndexerKeyFormat::make_contract_key_prefix(
      key1, address, IndexerKeyFormat::kEvent, 100);
  IndexerKeyFormat::make_contract_key_prefix(
      key2, address, IndexerKeyFormat::kEvent, 200);
  IndexerKeyFormat::make_contract_key_prefix(
      key3, address, IndexerKeyFormat::kEvent, 300);

  // Prefixes should be in lexicographic order
  EXPECT_LT(key1, key2);
  EXPECT_LT(key2, key3);
  EXPECT_LT(key1, key3);
}

TEST(KeyFormatTest, ContractKeyRoundTripsRecordKind)
{
  ContractAddress addr;
  std::ranges::fill(addr, 0x5A);
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x7B);
  const BlockHeight block = 4242;
  const uint32_t seq = 9;

  for (IndexerKeyFormat::RecordKind kind :
       {IndexerKeyFormat::kEvent, IndexerKeyFormat::kRootTx}) {
    std::string key;
    IndexerKeyFormat::make_contract_key(key, addr, kind, block, tx_hash, seq);
    EXPECT_EQ(key.size(), IndexerKeyFormat::kContractKeySize);

    auto parsed = IndexerKeyFormat::parse_contract_key(key);
    ASSERT_TRUE(parsed.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT above
    const auto &p = parsed.value();
    EXPECT_EQ(p.address, addr);
    EXPECT_EQ(p.kind, kind);
    EXPECT_EQ(p.block_height, block);
    EXPECT_EQ(p.tx_hash, tx_hash);
    EXPECT_EQ(p.sequence_number, seq);
  }

  // The kind byte sits right after the address, so EVENT and ROOT_TX rows
  // for the same address sort into disjoint contiguous ranges.
  std::string ev_key, rt_key;
  IndexerKeyFormat::make_contract_key(ev_key, addr, IndexerKeyFormat::kEvent,
                                      block, tx_hash, seq);
  IndexerKeyFormat::make_contract_key(rt_key, addr, IndexerKeyFormat::kRootTx,
                                      block, tx_hash, seq);
  EXPECT_LT(ev_key, rt_key);
}

} // namespace hogd
