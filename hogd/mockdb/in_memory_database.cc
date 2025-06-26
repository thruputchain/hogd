
#include "hogd/mockdb/in_memory_database.h"

#include <cassert>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

std::shared_ptr<const StorageItemWrapper>
InMemoryDatabase::get_post_block_storage_item(AddressAndKey const &addrkey)
{
  auto map = items_.take();
  if (auto it = map->find(addrkey); it != map->end()) {
    return it->second;
  }
  return nullptr;
}

std::optional<SHA256Hash>
InMemoryDatabase::get_post_block_deployment(ContractAddress const &addr)
{
  auto m = contract_deployments_.take();
  if (auto it = m->find(addr); it != m->end()) {
    return it->second;
  }

  return std::nullopt;
}

std::shared_ptr<const Contract>
InMemoryDatabase::get_post_block_contract(SHA256Hash const &hash)
{
  auto m = contract_creates_.take();
  if (auto it = m->find(hash); it != m->end()) {
    return it->second;
  }
  return nullptr;
}

} // namespace hogd
