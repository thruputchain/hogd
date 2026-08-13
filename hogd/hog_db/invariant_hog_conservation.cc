#include "hogd/hog_db/invariant_hog_conservation.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/status.h"
#include "hogd/ramdb/db_status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "types.hh"
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
                                          HogDbStatus<DBItem> const &before,
                                          HogDbStatus<DBItem> const &after)
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
  case Prefix::kAddressAuthData:
    handle_address_auth_data_item_change(before, after);
    return;
  case Prefix::kAddressMetadata:
    handle_address_metadata_item_change(before, after);
    return;
  case Prefix::kContractDataKey:
    handle_contract_data_item_change(key, before, after);
    return;
  case Prefix::kReplayCache:
    handle_replay_cache_item_change(key, before, after);
    return;
  case Prefix::kRetrocausalQueueBlock:
    // The cost of queue blocks is handled by the metadata recorded in
    // RetrocausalQueueMetadata for the queue. We cost queues as a whole
    // instead of individual blocks.
    return;
  }
  warn("unknown db item prefix: {}", std::to_underlying(prefix));
}

void
HogConservationInvariant::handle_balance_change(
    HogDbStatus<DBItem> const &before, HogDbStatus<DBItem> const &after)
{
  auto get_balance = [](HogDbStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == kNNInt128);
    CHECK(item.value().value.size() == sizeof(int128_xdr));
    auto encoded = std::span<const std::byte, sizeof(int128_xdr)>(
        item.value().value.data(), sizeof(int128_xdr));
    return int128_from_xdr_bytes(encoded);
  };

  auto before_balance = get_balance(before);
  auto after_balance = get_balance(after);

  int128_t before_cost =
      before.has_value() ? cost_model_.get_balance_cost() : 0;
  int128_t after_cost = after.has_value() ? cost_model_.get_balance_cost() : 0;
  update_total_supply(after_balance - before_balance + after_cost -
                      before_cost);
}

void
HogConservationInvariant::handle_address_metadata_item_change(
    HogDbStatus<DBItem> const &before, HogDbStatus<DBItem> const &after)
{
  auto get_storage_cost = [this](HogDbStatus<DBItem> const &item) -> int128_t {
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
    return cost_model_.get_deployment_cost(deployed_contract_hash);
  };
  auto before_storage_cost = get_storage_cost(before);
  auto after_storage_cost = get_storage_cost(after);

  update_total_supply(after_storage_cost - before_storage_cost);
}

void
HogConservationInvariant::handle_contract_data_item_change(
    std::span<const std::byte> key, HogDbStatus<DBItem> const &before,
    HogDbStatus<DBItem> const &after)
{
  auto get_storage_cost = [this](HogDbStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }

    switch (item.value().type) {
    case kNNInt128:
      return cost_model_.get_nnint_cost();
    case kBytes:
      return cost_model_.get_bytestring_cost(item.value().value.size());
    case kRetrocausalQueue:
      RetrocausalQueueMetadata metadata;
      try {
        x23::deserialize(&metadata, x23::BufViewEnd(item.value().value));
      } catch (x23::XdrException &e) {
        warn("failed to deserialize retrocausal queue metadata: {}", e.what());
        assert(false);
        return 0;
      }

      return cost_model_.get_retrocausal_queue_cost(metadata.live_entries);
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

  const auto supply_db_key = make_contract_data_key(
      ItemKey(kNativeTokenContractAddress, kNativeTokenTotalSupplyKey));
  if (!std::ranges::equal(key, supply_db_key)) {
    return;
  }
  auto value = [](HogDbStatus<DBItem> const &item) -> int128_t {
    if (!item) {
      return 0;
    }
    assert(item->type == kNNInt128);
    assert(item->value.size() == sizeof(int128_xdr));
    return int128_from_xdr_bytes(
        std::span<const std::byte, sizeof(int128_xdr)>(item->value.data(),
                                                       sizeof(int128_xdr)));
  };
  // The counter records newly issued HOG. Its positive delta exactly offsets
  // the corresponding balance credit in the conservation sum.
  update_total_supply(-(value(after) - value(before)));
}

void
HogConservationInvariant::handle_address_auth_data_item_change(
    HogDbStatus<DBItem> const &before, HogDbStatus<DBItem> const &after)
{
  auto get_storage_cost = [this](HogDbStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == 0);
    AddressAuthData auth_data;
    try {
      x23::deserialize(&auth_data, x23::BufViewEnd(item.value().value));
    } catch (x23::XdrException &e) {
      warn("failed to deserialize address auth data: {}", e.what());
      assert(false);
      return 0;
    }
    size_t active_keys = 0;
    for (size_t i = 0; i < kMaxSigners; ++i) {
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
HogConservationInvariant::handle_replay_cache_item_change(
    std::span<const std::byte> key, HogDbStatus<DBItem> const &before,
    HogDbStatus<DBItem> const &after)
{
  // Two distinct items share the kReplayCache prefix: the per-address metadata
  // item (key = prefix + address) and the per-height data items (key = prefix
  // + address + height). A replay cache's locked storage cost is a single
  // charge derived from the metadata's total nonce count (see
  // ReplayCacheLock::commit_change), so the whole cost is attributed to the
  // metadata item; the data items carry no independent locked cost.
  if (key.size() != kAddressSize + 1) {
    return;
  }

  auto get_storage_cost = [this](HogDbStatus<DBItem> const &item) -> int128_t {
    if (!item.has_value()) {
      return 0;
    }
    assert(item.value().type == 0);
    ReplayCacheMetadata meta;
    try {
      x23::deserialize(&meta, x23::BufViewEnd(item.value().value));
    } catch (x23::XdrException &e) {
      warn("failed to deserialize replay cache metadata: {}", e.what());
      assert(false);
      return 0;
    }
    return cost_model_.get_replay_cache_cost(static_cast<size_t>(meta.size));
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
