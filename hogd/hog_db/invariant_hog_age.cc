#include "hogd/hog_db/invariant_hog_age.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/log.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/status.h"
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
                                 HogDbStatus<DBItem> const &before,
                                 HogDbStatus<DBItem> const &after)
{
  auto prefix = static_cast<Prefix>(key[0]);
  auto delta = compute_delta(before, after);
  if (delta == ExistenceDelta::kUnchanged)
    return;

  switch (prefix) {
  case Prefix::kBalance:
    record_delta(key, delta, &Entry::balance);
    return;
  case Prefix::kHogAge:
    record_delta(key, delta, &Entry::hog_age);
    return;
  default:
    return;
  }
}

void
HogAgeInvariant::record_delta(std::span<const std::byte> key,
                              ExistenceDelta delta,
                              ExistenceDelta Entry::*field)
{
  auto raw = raw_address_from_key(key);
  entries_.with([&](auto &entries) { entries[raw].*field = delta; });
}

HogAgeInvariant::ExistenceDelta
HogAgeInvariant::compute_delta(HogDbStatus<DBItem> const &before,
                               HogDbStatus<DBItem> const &after)
{
  bool existed_before = before.has_value();
  bool exists_after = after.has_value();
  if (!existed_before && exists_after)
    return ExistenceDelta::kCreated;
  if (existed_before && !exists_after)
    return ExistenceDelta::kDeleted;
  return ExistenceDelta::kUnchanged;
}

RawAddress
HogAgeInvariant::raw_address_from_key(std::span<const std::byte> key)
{
  assert(key.size() == kAddressSize + 1);
  RawAddress raw{};
  std::ranges::copy(key.subspan(1),
                    std::begin(std::as_writable_bytes(std::span(raw))));
  return raw;
}

} // namespace hogd
