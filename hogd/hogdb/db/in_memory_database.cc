#include "hogd/hogdb/db/in_memory_database.h"

#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/hogexec/runtime/test_load_wasm.h"
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

void
InMemoryDatabase::set_as_default_genesis()
{
  auto wasm = test::load_wasm_from_file("genesis_deployer.wasm");
  auto hash = sha256(wasm);
  auto contract = std::make_shared<Contract>(wasm);
  contract_creates_->emplace(hash, contract);
  contract_deployments_->emplace(ContractAddress{}, hash);
}

} // namespace hogd
