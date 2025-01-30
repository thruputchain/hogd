#include "hogd/hogdb/db/in_memory_database.h"

#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/shared_util/load_contracts.h"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

std::optional<StorageItemWrapper>
InMemoryDatabase::get_post_block_storage_item(AddressAndKey const &addrkey)
{
  auto map = items_.take();
  if (auto it = map->find(addrkey); it != map->end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<SHA256Hash>
InMemoryDatabase::get_post_block_deployment(ContractAddress const &addr)
{
  std::printf("InMemoryDatabase::get_post_block_deployment %s\n",
              x23::show(addr).c_str());
  auto m = contract_deployments_.take();
  if (auto it = m->find(addr); it != m->end()) {
    return it->second;
  }

  return std::nullopt;
}

std::shared_ptr<const Contract>
InMemoryDatabase::get_post_block_contract(SHA256Hash const &hash)
{
  std::printf("InMemoryDatabase::get_post_block_contract %s\n",
              x23::show(hash).c_str());
  auto m = contract_creates_.take();
  if (auto it = m->find(hash); it != m->end()) {
    return it->second;
  }
  return nullptr;
}

void
InMemoryDatabase::set_as_default_genesis()
{
  auto [hash, contract] = load_built_rs_contract("genesis_deployer.wasm");
  contract_creates_->emplace(hash, contract);
  contract_deployments_->emplace(ContractAddress{}, hash);
}

} // namespace hogd
