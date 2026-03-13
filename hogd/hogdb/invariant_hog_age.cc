#include "hogd/hogdb/invariant_hog_age.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/log.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

bool
HogAgeInvariant::check()
{
  bool ok = true;
  entries_.with([&](auto &entries) {
    for (auto const &[raw_addr, entry] : entries) {
      if (entry.balance != entry.hog_age) {
        Address addr(raw_addr);
        error("hog age invariant violated for {}: balance delta={}, "
              "hog_age delta={}",
              addr, static_cast<int>(entry.balance),
              static_cast<int>(entry.hog_age));
        ok = false;
      }
    }
  });
  return ok;
}

void
HogAgeInvariant::log_item_change(std::span<const std::byte> key,
                                 HogDBStatus<DBItem> const &before,
                                 HogDBStatus<DBItem> const &after)
{
  auto prefix = static_cast<Prefix>(key[0]);

  switch (prefix) {
  case Prefix::kBalance:
  {
    auto delta = compute_delta(before, after);
    if (delta == ExistenceDelta::kUnchanged)
      return;
    auto raw = address_from_key(key).get_raw();
    entries_.with([&](auto &entries) { entries[raw].balance = delta; });
    return;
  }
  case Prefix::kHogAge:
  {
    auto delta = compute_delta(before, after);
    if (delta == ExistenceDelta::kUnchanged)
      return;
    auto raw = address_from_key(key).get_raw();
    entries_.with([&](auto &entries) { entries[raw].hog_age = delta; });
    return;
  }
  default:
    return;
  }
}

HogAgeInvariant::ExistenceDelta
HogAgeInvariant::compute_delta(HogDBStatus<DBItem> const &before,
                               HogDBStatus<DBItem> const &after)
{
  bool existed_before = before.has_value();
  bool exists_after = after.has_value();
  if (!existed_before && exists_after)
    return ExistenceDelta::kCreated;
  if (existed_before && !exists_after)
    return ExistenceDelta::kDeleted;
  return ExistenceDelta::kUnchanged;
}

Address
HogAgeInvariant::address_from_key(std::span<const std::byte> key)
{
  assert(key.size() == kAddressSize + 1);
  RawAddress raw{};
  std::ranges::copy(key.subspan(1),
                    std::begin(std::as_writable_bytes(std::span(raw))));
  return Address(raw);
}

} // namespace hogd
