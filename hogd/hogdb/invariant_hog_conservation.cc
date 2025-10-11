#include "hogd/hogdb/invariant_hog_conservation.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>

#include "account.hh"
#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

bool
HogConservationInvariant::check()
{
  auto r = total_supply_change_.with([](auto &t) { return t; });
  if (r != 0) {
    error("total_supply_change_ is not 0: {}", r);
  }
  return r == 0;
}

void
HogConservationInvariant::log_item_change(std::span<const std::byte> key,
                                          HogDBStatus<DBItem> const &before,
                                          HogDBStatus<DBItem> const &after)
{
  auto prefix = static_cast<Prefix>(key[0]);
  switch (prefix) {
  case Prefix::kBalance:
    handle_balance_change(before, after);
    return;
  case Prefix::kWasmCode:
  case Prefix::kGlobalMetadata:
  case Prefix::kHogAge:
  case Prefix::kWasmCodeRefcount:
    // This doesn't pay storage costs;
    return;
  case Prefix::kAccountAuthData:
    handle_account_auth_data_item_change(before, after);
    return;
  case Prefix::kAddressMetadata:
    handle_address_metadata_item_change(before, after);
    return;
  case Prefix::kContractDataKey:
    handle_contract_data_item_change(before, after);
    return;
  }
  warn("unknown db item prefix: {}", std::to_underlying(prefix));
}

void
HogConservationInvariant::handle_balance_change(
    HogDBStatus<DBItem> const &before, HogDBStatus<DBItem> const &after)
{
  auto get_balance = [](HogDBStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == kNNInt128);
    int128_t balance{0};
    std::ranges::copy(item.value().value, std::begin(std::as_writable_bytes(
                                              std::span(&balance, 1))));
    return le_to_cpu(balance);
  };
  auto before_balance = get_balance(before);
  auto after_balance = get_balance(after);
  update_total_supply(after_balance - before_balance);
}

void
HogConservationInvariant::handle_address_metadata_item_change(
    HogDBStatus<DBItem> const &before, HogDBStatus<DBItem> const &after)
{
  auto get_storage_cost = [this](HogDBStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == 0);
    DeployedContractHash deployed_contract_hash;
    try {
      x23::deserialize(&deployed_contract_hash,
                       x23::BufViewEnd(item.value().value));
    } catch (x23::XdrException &e) {
      warn("failed to deserialize deployed contract hash: {}", e.what());
      assert(false);
      return 0;
    }
    return cost_model_.get_contract_size_cost(
        deployed_contract_hash.contract_size);
  };
  auto before_storage_cost = get_storage_cost(before);
  auto after_storage_cost = get_storage_cost(after);

  update_total_supply(after_storage_cost - before_storage_cost);
}

void
HogConservationInvariant::handle_contract_data_item_change(
    HogDBStatus<DBItem> const &before, HogDBStatus<DBItem> const &after)
{
  auto get_storage_cost = [this](HogDBStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }

    switch (item.value().type) {
    case kNNInt128:
      return cost_model_.get_nnint_cost();
    case kBytes:
      return cost_model_.get_bytestring_cost(item.value().value.size());
    default:
      warn("unknown db item type: {}", item.value().type);
      assert(false);
      return 0;
    }
  };
  auto before_cost = get_storage_cost(before);
  auto after_cost = get_storage_cost(after);
  // These numbers are the amount of coins stored (locked) at this location.
  // Deleting an item (negative total supply change) is equivalent to
  // transferring that amount to another key
  update_total_supply(after_cost - before_cost);
}

void
HogConservationInvariant::handle_account_auth_data_item_change(
    HogDBStatus<DBItem> const &before, HogDBStatus<DBItem> const &after)
{
  auto get_storage_cost = [this](HogDBStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == 0);
    AccountAuthData auth_data;
    try {
      x23::deserialize(&auth_data, x23::BufViewEnd(item.value().value));
    } catch (x23::XdrException &e) {
      warn("failed to deserialize account auth data: {}", e.what());
      assert(false);
      return 0;
    }
    size_t active_keys = 0;
    for (size_t i = 0; i < MAX_SIGNERS; ++i) {
      if (auth_data.public_keys[i]) {
        active_keys++;
      }
    }
    return cost_model_.get_auth_data_cost(active_keys);
  };
  auto before_storage_cost = get_storage_cost(before);
  auto after_storage_cost = get_storage_cost(after);

  update_total_supply(after_storage_cost - before_storage_cost);
}

void
HogConservationInvariant::update_total_supply(int128_t change)
{
  [[maybe_unused]] auto r =
      total_supply_change_.with([change](auto &t) { return t += change; });
}

} // namespace hogd
