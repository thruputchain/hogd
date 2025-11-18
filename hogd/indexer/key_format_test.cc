#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "hogd/base/types.h"
#include "hogd/indexer/key_format.h"
#include "hogd/init.h"
#include "types.hh"

namespace hogd {

class IndexerKeyFormatTest : public ::testing::Test {
protected:
  void SetUp() override {}
};

// Test transaction key format - verify block_height is big-endian
TEST_F(IndexerKeyFormatTest, TxKeyByteOrder)
{
  BlockHeight block_height = 0x0123456789ABCDEF;
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0xAA);

  std::string key;
  IndexerKeyFormat::make_tx_key(key, block_height, tx_hash);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kTxKeySize);

  // Verify block_height is stored in big-endian order
  // First byte should be 0x01 (most significant byte)
  EXPECT_EQ(static_cast<uint8_t>(key[0]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(key[1]), 0x23);
  EXPECT_EQ(static_cast<uint8_t>(key[2]), 0x45);
  EXPECT_EQ(static_cast<uint8_t>(key[3]), 0x67);
  EXPECT_EQ(static_cast<uint8_t>(key[4]), 0x89);
  EXPECT_EQ(static_cast<uint8_t>(key[5]), 0xAB);
  EXPECT_EQ(static_cast<uint8_t>(key[6]), 0xCD);
  EXPECT_EQ(static_cast<uint8_t>(key[7]), 0xEF);

  // Verify tx_hash follows immediately after
  EXPECT_EQ(static_cast<uint8_t>(key[8]), 0xAA);
  EXPECT_EQ(static_cast<uint8_t>(key[39]), 0xAA);
}

// Test transaction key with zero block height
TEST_F(IndexerKeyFormatTest, TxKeyZeroHeight)
{
  BlockHeight block_height = 0;
  SHA256Hash tx_hash;
  std::fill(tx_hash.begin(), tx_hash.end(), 0xBB);

  std::string key;
  IndexerKeyFormat::make_tx_key(key, block_height, tx_hash);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kTxKeySize);

  // All 8 bytes of block_height should be zero
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0);
  }

  // Verify tx_hash follows
  EXPECT_EQ(static_cast<uint8_t>(key[8]), 0xBB);
}

// Test transaction key with maximum block height
TEST_F(IndexerKeyFormatTest, TxKeyMaxHeight)
{
  BlockHeight block_height = 0xFFFFFFFFFFFFFFFF;
  SHA256Hash tx_hash;
  std::fill(tx_hash.begin(), tx_hash.end(), 0xCC);

  std::string key;
  IndexerKeyFormat::make_tx_key(key, block_height, tx_hash);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kTxKeySize);

  // All 8 bytes of block_height should be 0xFF
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0xFF);
  }

  // Verify tx_hash follows
  EXPECT_EQ(static_cast<uint8_t>(key[8]), 0xCC);
}

// Test contract key prefix format - verify block_height is big-endian
TEST_F(IndexerKeyFormatTest, ContractKeyPrefixByteOrder)
{
  ContractAddress address;
  std::fill(address.begin(), address.end(), 0x11);
  BlockHeight block_height = 0x0123456789ABCDEF;

  std::string key;
  IndexerKeyFormat::make_contract_key_prefix(key, address, block_height);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kContractKeyPrefixSize);

  // Verify contract address is first (32 bytes)
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0x11);
  }

  // Verify block_height is stored in big-endian order after address
  EXPECT_EQ(static_cast<uint8_t>(key[32]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(key[33]), 0x23);
  EXPECT_EQ(static_cast<uint8_t>(key[34]), 0x45);
  EXPECT_EQ(static_cast<uint8_t>(key[35]), 0x67);
  EXPECT_EQ(static_cast<uint8_t>(key[36]), 0x89);
  EXPECT_EQ(static_cast<uint8_t>(key[37]), 0xAB);
  EXPECT_EQ(static_cast<uint8_t>(key[38]), 0xCD);
  EXPECT_EQ(static_cast<uint8_t>(key[39]), 0xEF);
}

// Test contract key format - verify block_height and sequence_number are
// big-endian
TEST_F(IndexerKeyFormatTest, ContractKeyByteOrder)
{
  ContractAddress address;
  std::fill(address.begin(), address.end(), 0x22);
  BlockHeight block_height = 0x0123456789ABCDEF;
  SHA256Hash tx_hash;
  std::fill(tx_hash.begin(), tx_hash.end(), 0x33);
  uint32_t sequence_number = 0x12345678;

  std::string key;
  IndexerKeyFormat::make_contract_key(key, address, block_height, tx_hash,
                                      sequence_number);

  EXPECT_EQ(key.size(), IndexerKeyFormat::kContractKeySize);

  // Verify contract address is first (32 bytes)
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0x22);
  }

  // Verify block_height is big-endian (bytes 32-39)
  EXPECT_EQ(static_cast<uint8_t>(key[32]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(key[33]), 0x23);
  EXPECT_EQ(static_cast<uint8_t>(key[34]), 0x45);
  EXPECT_EQ(static_cast<uint8_t>(key[35]), 0x67);
  EXPECT_EQ(static_cast<uint8_t>(key[36]), 0x89);
  EXPECT_EQ(static_cast<uint8_t>(key[37]), 0xAB);
  EXPECT_EQ(static_cast<uint8_t>(key[38]), 0xCD);
  EXPECT_EQ(static_cast<uint8_t>(key[39]), 0xEF);

  // Verify tx_hash follows (bytes 40-71)
  for (size_t i = 40; i < 72; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(key[i]), 0x33);
  }

  // Verify sequence_number is big-endian (bytes 72-75)
  EXPECT_EQ(static_cast<uint8_t>(key[72]), 0x12);
  EXPECT_EQ(static_cast<uint8_t>(key[73]), 0x34);
  EXPECT_EQ(static_cast<uint8_t>(key[74]), 0x56);
  EXPECT_EQ(static_cast<uint8_t>(key[75]), 0x78);
}

// Test contract key parsing round-trip
TEST_F(IndexerKeyFormatTest, ContractKeyRoundTrip)
{
  ContractAddress original_address;
  std::fill(original_address.begin(), original_address.end(), 0x44);
  BlockHeight original_height = 0x1122334455667788;
  SHA256Hash original_tx_hash;
  std::fill(original_tx_hash.begin(), original_tx_hash.end(), 0x55);
  uint32_t original_seq = 0xAABBCCDD;

  std::string key;
  IndexerKeyFormat::make_contract_key(key, original_address, original_height,
                                      original_tx_hash, original_seq);

  ContractAddress parsed_address;
  BlockHeight parsed_height;
  SHA256Hash parsed_tx_hash;
  uint32_t parsed_seq;

  bool success =
      IndexerKeyFormat::parse_contract_key(key, &parsed_address, &parsed_height,
                                           &parsed_tx_hash, &parsed_seq);

  ASSERT_TRUE(success);
  EXPECT_EQ(parsed_address, original_address);
  EXPECT_EQ(parsed_height, original_height);
  EXPECT_EQ(parsed_tx_hash, original_tx_hash);
  EXPECT_EQ(parsed_seq, original_seq);
}

// Test contract key parsing with null address pointer
TEST_F(IndexerKeyFormatTest, ContractKeyParseNullAddress)
{
  ContractAddress address;
  std::fill(address.begin(), address.end(), 0x66);
  BlockHeight height = 100;
  SHA256Hash tx_hash;
  std::fill(tx_hash.begin(), tx_hash.end(), 0x77);
  uint32_t seq = 5;

  std::string key;
  IndexerKeyFormat::make_contract_key(key, address, height, tx_hash, seq);

  BlockHeight parsed_height;
  SHA256Hash parsed_tx_hash;
  uint32_t parsed_seq;

  // Parse with null address pointer (should still work)
  bool success =
      IndexerKeyFormat::parse_contract_key(key, nullptr, &parsed_height,
                                           &parsed_tx_hash, &parsed_seq);

  ASSERT_TRUE(success);
  EXPECT_EQ(parsed_height, height);
  EXPECT_EQ(parsed_tx_hash, tx_hash);
  EXPECT_EQ(parsed_seq, seq);
}

// Test contract key parsing with wrong size
TEST_F(IndexerKeyFormatTest, ContractKeyParseWrongSize)
{
  std::string wrong_key = "too short";

  ContractAddress address;
  BlockHeight height;
  SHA256Hash tx_hash;
  uint32_t seq;

  bool success = IndexerKeyFormat::parse_contract_key(wrong_key, &address,
                                                      &height, &tx_hash, &seq);

  EXPECT_FALSE(success);
}

// Test that keys are lexicographically ordered by block_height
TEST_F(IndexerKeyFormatTest, TxKeyLexicographicOrder)
{
  SHA256Hash tx_hash;
  std::fill(tx_hash.begin(), tx_hash.end(), 0x88);

  std::string key1, key2, key3;
  IndexerKeyFormat::make_tx_key(key1, 100, tx_hash);
  IndexerKeyFormat::make_tx_key(key2, 200, tx_hash);
  IndexerKeyFormat::make_tx_key(key3, 300, tx_hash);

  // Keys should be in lexicographic order (big-endian ensures this)
  EXPECT_LT(key1, key2);
  EXPECT_LT(key2, key3);
  EXPECT_LT(key1, key3);
}

// Test that contract key prefixes are lexicographically ordered by block_height
TEST_F(IndexerKeyFormatTest, ContractKeyPrefixLexicographicOrder)
{
  ContractAddress address;
  std::fill(address.begin(), address.end(), 0x99);

  std::string key1, key2, key3;
  IndexerKeyFormat::make_contract_key_prefix(key1, address, 100);
  IndexerKeyFormat::make_contract_key_prefix(key2, address, 200);
  IndexerKeyFormat::make_contract_key_prefix(key3, address, 300);

  // Prefixes should be in lexicographic order
  EXPECT_LT(key1, key2);
  EXPECT_LT(key2, key3);
  EXPECT_LT(key1, key3);
}

} // namespace hogd
