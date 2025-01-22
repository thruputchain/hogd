#include "hogd/hogdb/softstate/transaction_cache.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

std::span<const std::byte>
TransactionCacheEntry::get_key() const
{
  return std::as_bytes(std::span(&txid_, 1));
}

std::span<const std::byte>
TransactionCache::get_key(TransactionID const &txid)
{
  return std::as_bytes(std::span(&txid, 1));
}

StorageLock
TransactionCache::get_lock(TransactionID txid, AddressAndKey const &addrkey)
{
  auto key = get_key(txid);
  auto res = tx_entries_.modify_element(key, [&addrkey](auto &v) {
    auto it = v.deltas_.find(addrkey);
    if (it == v.deltas_.end()) {
      return StorageLock(StorageLockType::kNone);
    }
    return it->second.lock;
  });
  if (!res) {
    return StorageLock(StorageLockType::kNone);
  }
  return *res;
}

void
TransactionCache::add_delta(TransactionID txid, AddressAndKey const &addrkey,
                            StorageDelta const &delta, StorageLock const &lock,
                            RPCFlagSet flags)
{
  auto key = get_key(txid);
  // no-op if element already exists
  tx_entries_.emplace(key, txid);

  [[maybe_unused]]
  auto modify_res = tx_entries_.modify_element(key, [&addrkey, &delta, &lock,
                                                     flags](auto &v) {
    v.deltas_[addrkey] = {
        .valid_for_final_commit =
            (flags & std::to_underlying(RPCFlags::kFinalCommit)) != 0,
        .delta = delta,
        .lock = lock,
    };
  });
  assert(modify_res);
}

std::unordered_map<AddressAndKey, LockedDeltaEntry>
TransactionCache::extract_deltas(TransactionID txid)
{
  auto key = get_key(txid);
  auto res = tx_entries_.modify_element(key, [](auto &v) {
    return std::move(v.deltas_);
  });
  if (res) {
    return *res;
  }
  return {};
}

} // namespace hogd
