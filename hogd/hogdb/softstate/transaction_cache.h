#pragma once

#include <unordered_map>

#include "hogd/base/hashmap.h"
#include "hogd/base/xdr_util.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

// Store data relevant to the 2PC process for one transaction.
class TransactionCacheEntry {
public:
  TransactionCacheEntry(TransactionID txid) : txid_(txid) {}
  std::span<const std::byte> get_key() const;

  std::unordered_map<AddressAndKey, std::pair<StorageDelta, StorageLock>>
      deltas_;

private:
  TransactionID txid_;
};

// map transaction id -> TransactionCacheEntry
class TransactionCache {
public:
  TransactionCache() : tx_entries_(8, 8) {}

  // Returns a lock for the given transaction and address.
  // Lock may not be valid if a concurrent operation performs an add_delta().
  // As such, no transaction should submit concurrent calls to
  // try_reserve_storage_delta()/add_delta(). (That behavior doesn't make sense
  // anyway).
  StorageLock get_lock(TransactionID txid, AddressAndKey const &addrkey);

  void add_delta(TransactionID txid, AddressAndKey const &addrkey,
                 StorageDelta const &delta, StorageLock const &lock);

  // Should be called only after a transaction has finished -- i.e.
  // after all add_delta() calls for the txid have completed.
  // This powers calls to commit/abort the transaction.
  std::unordered_map<AddressAndKey, std::pair<StorageDelta, StorageLock>>
  get_deltas(TransactionID txid);

private:
  HashMap<TransactionCacheEntry> tx_entries_;

  static std::span<const std::byte> get_key(TransactionID const &txid);
};

} // namespace hogd
