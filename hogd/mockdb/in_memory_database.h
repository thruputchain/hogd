
#pragma once

#include <cassert>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "account.hh"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

// In-memory database.
// Should be used only as a mock for testing.
// Does not manage view consistency.
class InMemoryDatabase {
public:
  // Should be called from within a StorageView.
  // This should return the storage item at the end of block immediately
  // preceeding a storage view that is a direct descendant of this.
  // (i.e. the most recently committed block)
  // However, this object does not manage view changes internally.
  //
  // As such, when a StorageView starts to write to this object,
  // it should no longer call get_post_block_storage_item(),
  // and it must not begin to write to this until it has committed --
  // and thus any siblings are deactivated.
  std::shared_ptr<const StorageItemWrapper>
  get_post_block_storage_item(AddressAndKey const &addrkey);
  std::optional<SHA256Hash>
  get_post_block_deployment(ContractAddress const &addr);
  std::shared_ptr<const Contract>
  get_post_block_contract(SHA256Hash const &hash);

  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> item)
  {
    items_->erase(addrkey);
    if (item) {
      items_->emplace(addrkey, std::move(item));
    }
  }

  void write_contract(SHA256Hash const &hash,
                      std::shared_ptr<const Contract> contract)
  {
    contract_creates_->emplace(hash, std::move(contract));
  }

  void write_deployment(ContractAddress const &addr,
                        DeployedContractHash const &deployment,
                        StorageCostModel const *cost_model)
  {
    write_item(
        AddressAndKey{.address = addr,
                      .key = StorageKey(
                          StorageKeyType::kStorageKeyTypeAddressMetadata)},
        std::make_shared<const StorageItemWrapper>(deployment, cost_model));
  }

  void write_deployment(ContractAddress const &addr,
                        std::shared_ptr<const Contract> const &contract)
  {
    auto contract_hash = sha256(*contract);
    uint32_t contract_size = 0;

    switch (contract->contract_type()) {
    case ContractType::kNative:
    case ContractType::kMockTest:
      contract_size = 0;
      break;
    case ContractType::kWasmi:
      contract_size =
          static_cast<uint32_t>(contract->wasmi_contract().wasm.size());
      break;
    default:
      assert(false);
      contract_size = 0;
      break;
    }

    write_deployment(addr,
                     DeployedContractHash{.hash = contract_hash,
                                          .contract_size = contract_size},
                     nullptr);
  }

private:
  x23::MVar<std::unordered_map<AddressAndKey,
                               std::shared_ptr<const StorageItemWrapper>>>
      items_;
  x23::MVar<std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>>>
      contract_creates_;
};

} // namespace hogd
