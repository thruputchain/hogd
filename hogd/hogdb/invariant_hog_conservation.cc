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
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "x23/serialize.h"
#include "x23/vec.h"


namespace hogd {

bool HogConservationInvariant::check() {
    return total_supply_change_ == 0;
}

void HogConservationInvariant::log_item_change(std::span<const std::byte> key, HogDBStatus<DBItem> const& before, HogDBStatus<DBItem> const& after) {
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

void HogConservationInvariant::handle_balance_change(HogDBStatus<DBItem> const& before, HogDBStatus<DBItem> const& after) {
    auto get_balance = [](HogDBStatus<DBItem> const& item) -> int128_t {
        if (!item.has_value()) {
            return 0;
        }
        assert(item.value().type == kNNInt128);
        int128_t balance{0};
        std::ranges::copy(item.value().value, std::begin(std::as_writable_bytes(std::span(&balance, 1))));
        return le_to_cpu(balance);
    };
    auto before_balance = get_balance(before);
    auto after_balance = get_balance(after);

    total_supply_change_ += after_balance - before_balance;
}

void HogConservationInvariant::handle_address_metadata_item_change(HogDBStatus<DBItem> const& before, HogDBStatus<DBItem> const& after) {
    auto get_storage_cost = [](HogDBStatus<DBItem> const& item) -> int128_t {
        if (!item.has_value()) {
            return 0;
        }
        assert(item.value().type == 0);
        DeployedContractHash deployed_contract_hash;
        try {
            x23::deserialize(&deployed_contract_hash, x23::BufViewEnd(item.value().value));
        } catch (x23::XdrException &e) {
            warn("failed to deserialize deployed contract hash: {}", e.what());
            assert(false);
            return 0;
        }
        return storage_cost(deployed_contract_hash.contract_size);
    };
    auto before_storage_cost = get_storage_cost(before);
    auto after_storage_cost = get_storage_cost(after);

    total_supply_change_ += after_storage_cost - before_storage_cost;
}

void HogConservationInvariant::handle_contract_data_item_change(HogDBStatus<DBItem> const& before, HogDBStatus<DBItem> const& after) {
    auto get_storage_cost = [](HogDBStatus<DBItem> const& item) -> int128_t {
        if (!item.has_value()) {
            return 0;
        }

        switch (item.value().type) {
            case kNNInt128:
                return storage_cost(sizeof(int128_t));
            case kBytes:
                return storage_cost(item.value().value.size());
            default:
                warn("unknown db item type: {}", item.value().type);
                assert(false);
                return 0;
        }
    };
    auto before_balance = get_storage_cost(before);
    auto after_balance = get_storage_cost(after);

    total_supply_change_ += after_balance - before_balance;
}

void HogConservationInvariant::handle_account_auth_data_item_change(HogDBStatus<DBItem> const& before, HogDBStatus<DBItem> const& after) {
    auto get_storage_cost = [](HogDBStatus<DBItem> const& item) -> int128_t {
        if (!item.has_value()) {
            return 0;
        }
    };
    auto before_storage_cost = get_storage_cost(before);
    auto after_storage_cost = get_storage_cost(after);

    total_supply_change_ += after_storage_cost - before_storage_cost;
}

}