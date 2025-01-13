#include "hogd/hogdb/softstate/transaction_cache.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "storage_item.hh"
#include "types.hh"

namespace hogd {

std::span<const std::byte>
TransactionCacheEntry::get_key() const
{
  return std::as_bytes(std::span(&txid_, 1));
}

std::span<const std::byte>
TransactionCache::get_key(TransactionID txid)
{
  return std::as_bytes(std::span(&txid, 1));
}

void
TransactionCache::add_delta(TransactionID txid, AddressAndKey const &addrkey,
                            StorageDelta const &delta)
{
  auto key = get_key(txid);
  // no-op if element already exists
  tx_entries_.emplace(key, txid);

  [[maybe_unused]]
  auto modify_res =
      tx_entries_.modify_element(key, [&addrkey, &delta](auto &v) {
        v.deltas_.emplace_back(addrkey, delta);
      });
  assert(modify_res);
}

std::vector<std::pair<AddressAndKey, StorageDelta>>
TransactionCache::get_deltas(TransactionID txid)
{
  auto key = get_key(txid);
  auto res =
      tx_entries_.modify_element(key, [](auto const &v) { return v.deltas_; });
  if (res) {
    return *res;
  }
  return {};
}

} // namespace hogd
