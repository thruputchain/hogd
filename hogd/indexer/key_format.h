#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "types.hh"

namespace hogd {

class IndexerKeyFormat {
public:
  IndexerKeyFormat() = delete;

  // Column family names for RocksDB
  static constexpr const char *kTxIndexColumnFamily = "tx_index";
  static constexpr const char *kContractIndexColumnFamily = "contract_index";

  // Transaction index key format:
  //   block_height (8 bytes BE) + tx_hash (32 bytes)
  // Total: 40 bytes
  // The intent is to support efficient range queries by block_height
  // (for efficient deletes of old data), and to efficiently query
  // for individual transactions.
  static constexpr size_t kTxKeySize = 8 + 32;

  // Contract index key format:
  //   contract_address (32 bytes) + block_height (8 bytes BE) +
  //   tx_hash (32 bytes) + sequence_number (4 bytes BE)
  // Total: 76 bytes
  static constexpr size_t kContractKeySize = 32 + 8 + 32 + 4;

  // Contract index key prefix format (for range queries):
  //   contract_address (32 bytes) + block_height (8 bytes BE)
  // Total: 40 bytes
  static constexpr size_t kContractKeyPrefixSize = 32 + 8;

  // Build a transaction index key into the provided buffer
  static void make_tx_key(std::string &key, BlockHeight block_height,
                          SHA256Hash const &tx_hash);

  // Build a contract index key prefix into the provided buffer
  static void make_contract_key_prefix(std::string &key,
                                       ContractAddress const &address,
                                       BlockHeight block_height);

  // Build a contract index key into the provided buffer
  static void make_contract_key(std::string &key,
                                ContractAddress const &address,
                                BlockHeight block_height,
                                SHA256Hash const &tx_hash,
                                uint32_t sequence_number);

  // Parse a contract key and extract components
  // Returns true if parsing succeeded, false otherwise
  static bool parse_contract_key(std::string_view key, ContractAddress *address,
                                 BlockHeight *block_height, SHA256Hash *tx_hash,
                                 uint32_t *sequence_number);
};

} // namespace hogd
