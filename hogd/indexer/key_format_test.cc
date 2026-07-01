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

  ContractAddress parsed_address;
  IndexerKeyFormat::RecordKind parsed_kind;
  BlockHeight parsed_height;
  SHA256Hash parsed_tx_hash;
  uint32_t parsed_seq;

  bool success = IndexerKeyFormat::parse_contract_key(
      key, &parsed_address, &parsed_kind, &parsed_height, &parsed_tx_hash,
      &parsed_seq);

  ASSERT_TRUE(success);
  EXPECT_EQ(parsed_address, original_address);
  EXPECT_EQ(parsed_kind, IndexerKeyFormat::kEvent);
  EXPECT_EQ(parsed_height, original_height);
  EXPECT_EQ(parsed_tx_hash, original_tx_hash);
  EXPECT_EQ(parsed_seq, original_seq);
}

// Test contract key parsing with null address pointer
TEST_F(IndexerKeyFormatTest, ContractKeyParseNullAddress)
{
  ContractAddress address;
  std::ranges::fill(address, 0x66);
  BlockHeight height = 100;
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x77);
  uint32_t seq = 5;

  std::string key;
  IndexerKeyFormat::make_contract_key(key, address, IndexerKeyFormat::kEvent,
                                      height, tx_hash, seq);

  BlockHeight parsed_height;
  SHA256Hash parsed_tx_hash;
  uint32_t parsed_seq;

  // Parse with null address and kind pointers (should still work)
  bool success = IndexerKeyFormat::parse_contract_key(
      key, nullptr, nullptr, &parsed_height, &parsed_tx_hash, &parsed_seq);

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
  IndexerKeyFormat::RecordKind kind;
  BlockHeight height;
  SHA256Hash tx_hash;
  uint32_t seq;

  bool success = IndexerKeyFormat::parse_contract_key(
      wrong_key, &address, &kind, &height, &tx_hash, &seq);

  EXPECT_FALSE(success);
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

    ContractAddress addr_out;
    IndexerKeyFormat::RecordKind kind_out;
    BlockHeight block_out;
    SHA256Hash tx_out;
    uint32_t seq_out;
    ASSERT_TRUE(IndexerKeyFormat::parse_contract_key(
        key, &addr_out, &kind_out, &block_out, &tx_out, &seq_out));
    EXPECT_EQ(addr_out, addr);
    EXPECT_EQ(kind_out, kind);
    EXPECT_EQ(block_out, block);
    EXPECT_EQ(tx_out, tx_hash);
    EXPECT_EQ(seq_out, seq);
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
