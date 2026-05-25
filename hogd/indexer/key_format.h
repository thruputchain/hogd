#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "types.hh"

namespace hogd {

static_assert('A' == 0x41 && '0' == 0x30 && '_' == 0x5F,
              "ordinary literal encoding must be ASCII-compatible");

class IndexerKeyFormat {
public:
  IndexerKeyFormat() = delete;

  // Scheme format for this index:
  // Column family: tx_index
  // This is the primary index, and is responsible for storing the list of
  // events per each transaction. Key format:
  //   tx_hash (32 bytes) (total: 32 bytes)
  // Value format:
  //   serialized TxEventLog
  // The first bytes of the key are the block height, so that appending new elts
  // and pruning old data is efficient.
  //
  // This index is the primary event index.  Other indices do not store copies
  // of the events, but rather just reference things that can be looked up in
  // this index.

  // Column family: contract_index
  // This index is responsible for allowing searches of events by contract
  // address. Key format:
  //   contract_address (32 bytes) + block_height (8 bytes BE) + tx_hash (32
  //   bytes) + sequence_number (4 bytes BE) (total: 76 bytes)
  // Value format:
  //   empty string (event data is looked up from tx_index using block_height
  //   and tx_hash)
  //
  // This index provides a way to find events by contract address. The actual
  // event data is retrieved by looking up the transaction in tx_index using the
  // block_height and tx_hash from the key, then extracting the event at the
  // sequence_number.

  // Column family names for RocksDB
  static constexpr const char *kTxIndexColumnFamily = "tx_index";
  static constexpr const char *kContractIndexColumnFamily = "contract_index";

  // Default-CF meta key holding the highest block_height ever observed
  // by consume_events, encoded as 8 bytes big-endian.  Read once on
  // open to seed [highest_indexed_block_]; updated on every advance
  // inside consume_events.  Absent on databases that predate this key.
  static constexpr const char *kHighestIndexedBlockMetaKey =
      "meta:highest_indexed_block";

  // Transaction index key format:
  //   tx_hash (32 bytes)
  // Total: 32 bytes
  static constexpr size_t kTxKeySize = 32;

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
  static void make_tx_key(std::string &key, SHA256Hash const &tx_hash);

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
