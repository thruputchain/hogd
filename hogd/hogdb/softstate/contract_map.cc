#include "hogd/hogdb/softstate/contract_map.h"

#include <cassert>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/crypto/crypto.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/mvar.h"

namespace hogd {

std::optional<SHA256Hash>
ContractMap::get_deployment(ContractAddress const &addr)
{
  auto d_map = deployment_map_.take();
  if (auto it = d_map->find(addr); it != d_map->end()) {
    return it->second;
  }
  return std::nullopt;
}
std::shared_ptr<const Contract>
ContractMap::get_contract(SHA256Hash const &hash)
{
  auto c_map = contract_map_.take();
  if (auto it = c_map->find(hash); it != c_map->end()) {
    return it->second.contract_;
  }
  return nullptr;
}

void
ContractMap::deploy_contract(ContractAddress const &addr,
                             SHA256Hash const &hash)
{
  auto new_d_map = new_deployment_map_.take();

  if (auto it = new_d_map->find(addr); it != new_d_map->end()) {
    new_d_map->emplace(addr, hash);
    assert(!deployment_map_->contains(addr));
    bump_contract_refcount(hash);
  }
}

void
ContractMap::bump_contract_refcount(SHA256Hash const &hash)
{
  auto new_c_map = new_contract_map_.take();

  if (auto it = new_c_map->find(hash); it != new_c_map->end()) {
    it->second.deployment_count_++;
    return;
  }

  auto old_c_map = contract_map_.take();
  auto it = old_c_map->find(hash);

  if (it == old_c_map->end()) {
    new_c_map->emplace(hash, RegisteredContract{.contract_ = nullptr,
                                                .deployment_count_ = 1});
    return;
  }

  auto new_contract_entry = it->second;
  new_contract_entry.deployment_count_++;
  new_c_map->emplace(hash, new_contract_entry);
}

void
ContractMap::add_contract(std::shared_ptr<const Contract> contract)
{
  auto contract_hash = sha256(*contract);
  auto new_c_map = new_contract_map_.take();

  if (auto it = new_c_map->find(contract_hash); it != new_c_map->end()) {
    assert(it->second.contract_ == nullptr);
    it->second.contract_ = contract;
  }
  else {
    new_c_map->emplace(contract_hash,
                       RegisteredContract{.contract_ = contract,
                                          .deployment_count_ = 0});
  }
}

void
ContractMap::commit_block()
{
  auto d_map = deployment_map_.take();
  auto new_d_map = new_deployment_map_.take();

  for (auto const &[addr, hash] : *new_d_map) {
    // Does not matter if deployment is already present.
    d_map->emplace(addr, hash);
  }

  auto c_map = contract_map_.take();
  auto new_c_map = new_contract_map_.take();

  for (auto const &[hash, contract] : *new_c_map) {
    // Intentionally overwrite reference counts
    c_map->erase(hash);
    c_map->emplace(hash, contract);
  }
}

void
ContractMap::revert_block()
{
  new_deployment_map_->clear();
  new_contract_map_->clear();
}

x23::Async<RPCError>
ContractStorageServer::commit_contract_create(Contract const &new_contract)
{
  contract_map_.add_contract(std::make_shared<const Contract>(new_contract));
  co_return RPCError::kNone;
}

x23::Async<RPCError>
ContractStorageServer::commit_contract_deploy(
    ContractAddress const &new_address,
    SHA256Hash const &deployed_contract_hash)
{
  contract_map_.deploy_contract(new_address, deployed_contract_hash);
  co_return RPCError::kNone;
}

x23::Async<ExpectedContract>
ContractStorageServer::get_contract(SHA256Hash const &contract_hash)
{
  auto res = contract_map_.get_contract(contract_hash);
  if (res) {
    ExpectedContract out(RPCError::kNone);
    out.contract() = *res;
    co_return out;
  }
  co_return ExpectedContract(RPCError::kContractNExist);
}

x23::Async<ExpectedDeployment>
ContractStorageServer::get_deployment(ContractAddress const &contract_address)
{
  auto res = contract_map_.get_deployment(contract_address);
  if (res) {
    ExpectedDeployment out(RPCError::kNone);
    out.deployed_hash() = *res;
    co_return out;
  }
  co_return ExpectedDeployment(RPCError::kNoContractDeployed);
}

} // namespace hogd
