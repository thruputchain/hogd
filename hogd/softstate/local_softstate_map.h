#pragma once

#include <map>
#include <optional>

#include "contract.hh"
#include "hogd/softstate/storage_object_frame.h"
#include "storage_interface.hh"
#include "storage_object.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/mvar.h"

namespace hogd {

class LocalSoftstateMap {
public:
  StorageReserveResult try_reserve_delta(AddressAndKey const &addrkey,
                                         StorageDelta const &delta);
  void commit_delta(AddressAndKey const &addrkey, StorageDelta const &delta);
  void revert_delta(AddressAndKey const &addrkey, StorageDelta const &delta);

  std::optional<StorageObject>
  get_committed_object(AddressAndKey const &addrkey);

  void commit_round();
  void revert_round();

private:
  x23::MVar<std::map<AddressAndKey, StorageObjectFrame>> map_;
};

class LocalContractMap {
public:
  std::optional<SHA256Hash> get_deployment(ContractAddress const &addr);
  std::shared_ptr<const Contract> get_contract(SHA256Hash const &hash);

  void deploy_contract(ContractAddress const &addr, SHA256Hash const &hash);
  void add_contract(SHA256Hash const &hash,
                    std::shared_ptr<const Contract> contract);

  void commit_round();
  void revert_round();

private:
  x23::MVar<std::map<ContractAddress, SHA256Hash>> deployment_map_;
  x23::MVar<std::map<SHA256Hash, std::shared_ptr<const Contract>>>
      contract_map_;

  x23::MVar<std::map<ContractAddress, SHA256Hash>> new_deployment_map_;
  x23::MVar<std::map<SHA256Hash, std::shared_ptr<const Contract>>>
      new_contract_map_;
};

class LocalSoftstateReadServer {
public:
  using rpc_service = StorageReadInterfaceV1;

  LocalSoftstateReadServer(LocalSoftstateMap &map, LocalContractMap &contracts)
    : local_map_(map), local_contract_map_(contracts)
  {}

  x23::Async<ExpectedStorageObject>
  get_object(SHA256Hash const &expected_block_hash,
             AddressAndKey const &addrkey);

  x23::Async<ExpectedContract> get_contract(SHA256Hash const &hash);

  x23::Async<ExpectedDeployment> get_deployment(ContractAddress const &addr);

private:
  LocalSoftstateMap &local_map_;
  LocalContractMap &local_contract_map_;
};

static_assert(
    is_StorageReadInterfaceV1<LocalSoftstateReadServer *, x23::Async>);

class LocalSoftstateWriteServer {
public:
  using rpc_service = StorageWriteInterfaceV1;

  LocalSoftstateWriteServer(LocalSoftstateMap &map, LocalContractMap &contracts)
    : local_map_(map), local_contract_map_(contracts)
  {}

  x23::Async<StorageRPCError>
  try_reserve_transaction_deltas(StorageDeltaVecList const &deltas,
                                 SHA256Hash const &expected_block_hash);

  x23::Async<StorageRPCError>
  revert_transaction_deltas(StorageDeltaVecList const &deltas,
                            SHA256Hash const &expected_block_hash);

  x23::Async<StorageRPCError>
  commit_transaction_deltas(StorageDeltaVecList const &deltas,
                            SHA256Hash const &expected_block_hash);

  x23::Async<StorageRPCError>
  commit_contract_create(Contract const &new_contract,
                         SHA256Hash const &expected_block_hash);

  x23::Async<StorageRPCError>
  commit_contract_deploy(ContractAddress const &new_contract_addr,
                         SHA256Hash const &deployed_contract_hash,
                         SHA256Hash const &expected_block_hash);

private:
  StorageRPCError try_reserve_transaction_deltas(StorageDeltaVec const &dv);
  void revert_transaction_deltas(StorageDeltaVec const &dv);

  LocalSoftstateMap &local_map_;
  LocalContractMap &local_contract_map_;
};

static_assert(
    is_StorageWriteInterfaceV1<LocalSoftstateWriteServer *, x23::Async>);

} // namespace hogd
