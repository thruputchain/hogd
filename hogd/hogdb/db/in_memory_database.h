#pragma once

#include <unordered_map>

#include "contract.hh"
#include "hogd/base/xdr_util.h"
#include "storage_item.hh"
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
  // However, this object does not manage view changes internally.
  //
  // As such, when a StorageView starts to write to this object,
  // it should no longer call get_post_block_storage_item(),
  // and it must not begin to write to this until it has committed --
  // and thus any siblings are deactivated.
  std::optional<StorageItem>
  get_post_block_storage_item(AddressAndKey const &addrkey);
  std::optional<SHA256Hash>
  get_post_block_deployment(ContractAddress const &addr);
  std::shared_ptr<const Contract>
  get_post_block_contract(SHA256Hash const &hash);

  void write_item(AddressAndKey const &addrkey,
                  std::optional<StorageItem> const &item)
  {
    if (item) {
      items_->emplace(addrkey, *item);
    }
    else {
      items_->erase(addrkey);
    }
  }

  void write_contract(SHA256Hash const &hash,
                      std::shared_ptr<const Contract> contract)
  {
    contract_creates_->emplace(hash, contract);
  }

  void write_deployment(ContractAddress const &addr,
                        SHA256Hash const &contract_hash)
  {
    contract_deployments_->emplace(addr, contract_hash);
  }

private:
  x23::MVar<std::unordered_map<AddressAndKey, StorageItem>> items_;
  x23::MVar<std::unordered_map<ContractAddress, SHA256Hash>>
      contract_deployments_;
  x23::MVar<std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>>>
      contract_creates_;
};

} // namespace hogd
