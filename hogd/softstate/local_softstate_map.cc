#include "hogd/softstate/local_softstate_map.h"

#include <cassert>

#include "storage_object.hh"

namespace hogd {

StorageReserveResult
LocalSoftstateMap::try_reserve_delta(AddressAndKey const &addrkey,
                                     StorageDelta const &delta)
{
  auto ptr = map_.take();

  auto it = ptr->find(addrkey);
  if (it == ptr->end()) {
    it = ptr->emplace(addrkey, std::nullopt).first;
  }

  return it->second.try_reserve_delta(delta);
}

void
LocalSoftstateMap::commit_delta(AddressAndKey const &addrkey,
                                StorageDelta const &delta)
{
  auto ptr = map_.take();

  auto it = ptr->find(addrkey);
  assert(it != ptr->end());

  it->second.commit_delta(delta);
}

void
LocalSoftstateMap::revert_delta(AddressAndKey const &addrkey,
                                StorageDelta const &delta)
{
  auto ptr = map_.take();

  auto it = ptr->find(addrkey);
  assert(it != ptr->end());

  it->second.revert_delta(delta);
}

void
LocalSoftstateMap::commit_round()
{
  auto ptr = map_.take();
  for (auto it = ptr->begin(); it != ptr->end(); it++) {
    it->second.commit_round();
  }
}

void
LocalSoftstateMap::revert_round()
{
  auto ptr = map_.take();
  for (auto it = ptr->begin(); it != ptr->end(); it++) {
    it->second.revert_round();
  }
}

std::optional<StorageObject>
LocalSoftstateMap::get_committed_object(AddressAndKey const &addrkey)
{
  auto ptr = map_.take();
  auto it = ptr->find(addrkey);
  if (it == ptr->end()) {
    return std::nullopt;
  }

  return it->second.get_committed_object();
}

std::optional<SHA256Hash>
LocalContractMap::get_deployment(ContractAddress const &addr)
{
  auto ptr = deployment_map_.take();

  auto it = ptr->find(addr);
  if (it == ptr->end()) {
    return std::nullopt;
  }
  return it->second;
}

std::shared_ptr<const Contract>
LocalContractMap::get_contract(SHA256Hash const &hash)
{
  auto ptr = contract_map_.take();

  auto it = ptr->find(hash);
  if (it == ptr->end()) {
    return nullptr;
  }
  return it->second;
}

void
LocalContractMap::deploy_contract(ContractAddress const &addr,
                                  SHA256Hash const &hash)
{
  deployment_map_->emplace(addr, hash);
}

void
LocalContractMap::add_contract(SHA256Hash const &hash,
                               std::shared_ptr<const Contract> contract)
{
  contract_map_->emplace(hash, contract);
}

x23::Async<ExpectedStorageObject>
LocalSoftstateReadServer::get_object(SHA256Hash const &expected_block_hash,
                                     AddressAndKey const &addrkey)
{
  ExpectedStorageObject out;
  auto res = local_map_.get_committed_object(addrkey);

  if (!res) {
    out.error(StorageRPCError::kAddressAndKeyNExist);
  }
  else {
    out.error(StorageRPCError::kNone);
    out.object() = *res;
  }
  co_return out;
}

x23::Async<ExpectedContract>
LocalSoftstateReadServer::get_contract(SHA256Hash const &hash)
{
  ExpectedContract out;
  auto res = local_contract_map_.get_contract(hash);

  if (!res) {
    out.error(StorageRPCError::kContractNExist);
  }
  else {
    out.error(StorageRPCError::kNone);
    out.contract() = *res;
  }
  co_return out;
}

x23::Async<ExpectedDeployment>
LocalSoftstateReadServer::get_deployment(ContractAddress const &addr)
{
  ExpectedDeployment out;
  auto res = local_contract_map_.get_deployment(addr);

  if (!res) {
    out.error(StorageRPCError::kNoContractDeployed);
  }
  else {
    out.error(StorageRPCError::kNone);
    out.deployed_hash() = *res;
  }
  co_return out;
}

x23::Async<StorageRPCError>
LocalSoftstateWriteServer::try_reserve_transaction_deltas(
    StorageDeltaVecList const &deltas, SHA256Hash const &expected_block_hash)
{
  bool failed = false;
  ssize_t i = 0;
  ssize_t j = 0;

  for (; i < deltas.size(); i++) {
    auto const &dv = deltas[i];
    for (j = 0; j < dv.deltas.size(); j++) {
      auto res = local_map_.try_reserve_delta(dv.addr_key, dv.deltas[j]);

      if (res != StorageReserveResult::kSuccess) {
        failed = true;
        // the failed one is a no-op
        break;
      }
    }
    if (failed) {
      break;
    }
  }

  if (failed) {
    // cleanup
    for (ssize_t ii = 0; ii <= i; ii++) {
      auto const &dv = deltas[ii];

      ssize_t jmax = dv.deltas.size();
      if (ii == i) {
        jmax = j - 1;
      }

      for (ssize_t jj = 0; jj < jmax; jj++) {
        local_map_.revert_delta(dv.addr_key, dv.deltas[jj]);
      }
    }
    co_return StorageRPCError::kApplyFailed;
  }
  co_return StorageRPCError::kNone;
}

x23::Async<StorageRPCError>
LocalSoftstateWriteServer::revert_transaction_deltas(
    StorageDeltaVecList const &deltas, SHA256Hash const &expected_block_hash)
{
  for (auto const &dv : deltas) {
    for (auto const &delta : dv.deltas) {
      local_map_.revert_delta(dv.addr_key, delta);
    }
  }
  co_return StorageRPCError::kNone;
}

x23::Async<StorageRPCError>
LocalSoftstateWriteServer::commit_transaction_deltas(
    StorageDeltaVecList const &deltas, SHA256Hash const &expected_block_hash)
{
  for (auto const &dv : deltas) {
    for (auto const &delta : dv.deltas) {
      local_map_.commit_delta(dv.addr_key, delta);
    }
  }
  co_return StorageRPCError::kNone;
}

} // namespace hogd
