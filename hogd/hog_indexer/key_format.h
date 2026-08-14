#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
  //   tx_hash (32 bytes) + block_height (8 bytes BE) (total: 40 bytes)
  // Value format:
  //   serialized TxEventLog
  //
  // The block height is part of the key (not just the value) because a single
  // tx_hash can be committed at more than one height -- e.g. an initial-token-
  // claim replay that is exempt from the replay cache and deterministically
  // errors on the second attempt.  Keying by hash alone let the later receipt
  // silently overwrite the earlier one, which then mismatched the
  // contract_index rows that still referenced the original height.  Appending
  // the height keeps each receipt addressable and sorts a hash's receipts by
  // ascending height.
  //
  // Databases written by binaries that predate the suffix still hold bare
  // 32-byte hash keys.  The read paths accept both layouts (the tx hash is the
  // leading 32 bytes either way), so an in-place upgrade keeps serving old
  // history without a rebuild; only new receipts get the block suffix.
  //
  // This index is the primary event index.  Other indices do not store copies
  // of the events, but rather just reference things that can be looked up in
  // this index.

  // Column family: contract_index
  // This index is responsible for allowing searches of events by contract
  // address. Key format:
  //   contract_address (32 bytes) + kind (1 byte) + block_height (8 bytes
  //   BE) + tx_hash (32 bytes) + sequence_number (4 bytes BE) (total: 77
  //   bytes)
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
  //   tx_hash (32 bytes) + block_height (8 bytes BE)
  // Total: 40 bytes
  static constexpr size_t kTxKeySize = 32 + 8;

  // Transaction index key prefix (all receipts for one tx_hash):
  //   tx_hash (32 bytes)
  // Total: 32 bytes
  static constexpr size_t kTxKeyPrefixSize = 32;

  // Record kind discriminator, stored as 1 byte immediately after the
  // address so each (address, kind) forms a contiguous key range.
  enum RecordKind : uint8_t {
    kEvent = 0,  // one row per emitted event, keyed by emitter address
    kRootTx = 1, // one row per tx, keyed by root invoked_address (seq = 0)
  };

  // Contract index key format:
  //   contract_address (32) + kind (1) + block_height (8 BE) +
  //   tx_hash (32) + sequence_number (4 BE)
  // Total: 77 bytes
  static constexpr size_t kContractKeySize = 32 + 1 + 8 + 32 + 4;

  // Contract index key prefix (for range queries):
  //   contract_address (32) + kind (1) + block_height (8 BE)
  // Total: 41 bytes
  static constexpr size_t kContractKeyPrefixSize = 32 + 1 + 8;

  // Build a transaction index key (tx_hash + block_height) into the buffer
  static void make_tx_key(std::string &key, SHA256Hash const &tx_hash,
                          BlockHeight block_height);

  // Build a transaction index key prefix (tx_hash only) into the buffer.
  // Used to seek every receipt committed under a given tx_hash.
  static void make_tx_key_prefix(std::string &key, SHA256Hash const &tx_hash);

  // Build a contract index key prefix into the provided buffer
  static void make_contract_key_prefix(std::string &key,
                                       ContractAddress const &address,
                                       RecordKind kind,
                                       BlockHeight block_height);

  // Build a contract index key into the provided buffer
  static void make_contract_key(std::string &key,
                                ContractAddress const &address, RecordKind kind,
                                BlockHeight block_height,
                                SHA256Hash const &tx_hash,
                                uint32_t sequence_number);

  // Decoded components of a contract index key.
  struct ParsedContractKey {
    ContractAddress address;
    RecordKind kind;
    BlockHeight block_height;
    SHA256Hash tx_hash;
    uint32_t sequence_number;
  };

  // Parse a contract index key.  Returns the decoded components, or
  // std::nullopt if [key] is not exactly kContractKeySize bytes.  Callers
  // read whichever fields they need and ignore the rest.
  static std::optional<ParsedContractKey>
  parse_contract_key(std::string_view key);
};

} // namespace hogd
