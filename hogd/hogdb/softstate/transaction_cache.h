#pragma once

#include "hogd/base/hashmap.h"
#include "rpc_types.hh"
#include "storage_item.hh"

namespace hogd {

class TransactionCacheEntry {
public:
  TransactionCacheEntry(TransactionID txid) : txid_(txid) {}
  std::span<const std::byte> get_key() const;

  std::vector<std::pair<AddressAndKey, StorageDelta>> deltas_;

private:
  TransactionID txid_;
};

class TransactionCache {
public:
  TransactionCache() : tx_entries_(8, 8) {}

  void add_delta(TransactionID txid, AddressAndKey const &addrkey,
                 StorageDelta const &delta);

  // Should be called only after a transaction has finished -- i.e.
  // after all add_delta() calls for the txid have completed.
  // This powers calls to commit/abort the transaction.
  std::vector<std::pair<AddressAndKey, StorageDelta>>
  get_deltas(TransactionID txid);

private:
  HashMap<TransactionCacheEntry> tx_entries_;

  static std::span<const std::byte> get_key(TransactionID txid);
};

} // namespace hogd
