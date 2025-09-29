#include "hogd/hogdb/invariant_contract_refcount.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <span>
#include <stdexcept>
#include <utility>

#include "account.hh"
#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

bool
HogContractRefcountInvariant::check()
{
  auto r = missing_contract_refcounts_.take();
  for (auto &[hash, refcount] : *r) {
    if (refcount != 0) {
      error("missing contract refcount: {} {}", hash, refcount);
      return false;
    }
  }
  return true;
}

void
HogContractRefcountInvariant::log_item_change(std::span<const std::byte> key,
                                              HogDBStatus<DBItem> const &before,
                                              HogDBStatus<DBItem> const &after)
{
  auto prefix = static_cast<Prefix>(key[0]);
  switch (prefix) {
  case Prefix::kWasmCode:
  case Prefix::kGlobalMetadata:
  case Prefix::kHogAge:
  case Prefix::kBalance:
  case Prefix::kAccountAuthData:
  case Prefix::kContractDataKey:
    // nothing to do here
    return;
  case Prefix::kWasmCodeRefcount:
    handle_wasm_refcount_change(key, before, after);
    // This doesn't pay storage costs;
    return;
  case Prefix::kAddressMetadata:
    handle_address_metadata_item_change(before, after);
    return;
  }
  warn("unknown db item prefix: {}", std::to_underlying(prefix));
}

void
HogContractRefcountInvariant::handle_wasm_refcount_change(
    std::span<const std::byte> key, HogDBStatus<DBItem> const &before,
    HogDBStatus<DBItem> const &after)
{
  SHA256Hash hash;
  assert(key.size() == hash.size() + 1);
  std::ranges::copy(key.subspan(1),
                    std::begin(std::as_writable_bytes(std::span(hash))));
  auto get_refcount = [](HogDBStatus<DBItem> const &item) -> int32_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == kNNInt128);
    int32_t refcount{0};
    std::ranges::copy(item.value().value, std::begin(std::as_writable_bytes(
                                              std::span(&refcount, 1))));
    return le_to_cpu(refcount);
  };
  auto before_refcount = get_refcount(before);
  auto after_refcount = get_refcount(after);
  update_refcount(hash, after_refcount - before_refcount);
}

void
HogContractRefcountInvariant::handle_address_metadata_item_change(
    HogDBStatus<DBItem> const &before, HogDBStatus<DBItem> const &after)
{
  auto get_refcount = [](HogDBStatus<DBItem> const &item) -> int32_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == 0);
    return 1;
  };
  auto before_refcount = get_refcount(before);
  auto after_refcount = get_refcount(after);

  DeployedContractHash hash;
  assert(after.value().type == 0);
  try {
    x23::deserialize(&hash, x23::BufViewEnd(after.value().value));
  } catch (x23::XdrException &e) {
    warn("failed to deserialize deployed contract hash: {}", e.what());
    assert(false);
    return;
  }

  // negative of the refcount change; this change counteracts the changes
  // logged in handle_wasm_refcount_change
  update_refcount(hash.hash, before_refcount - after_refcount);
}

void
HogContractRefcountInvariant::update_refcount(SHA256Hash const &hash,
                                              int32_t delta)
{
  missing_contract_refcounts_.with(
      [hash, delta](auto &t) { t[hash] += delta; });
}

} // namespace hogd
