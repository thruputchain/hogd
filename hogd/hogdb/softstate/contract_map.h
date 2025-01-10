#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "contract.hh"
#include "hogd/base/xdr_util.h"
#include "rpc.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/mvar.h"

namespace hogd {

// Maintain a map of all currently active contracts and deployments.
//
// All operations that modify state are either committed or reverted
// at the end of a block.
class ContractMap {
public:
  // Read from snapshot data, not data created by concurrent operations.
  std::optional<SHA256Hash> get_deployment(ContractAddress const &addr);
  std::shared_ptr<const Contract> get_contract(SHA256Hash const &hash);

  // Make a modification to contract existence or deployment within the current
  // block A transaction should never deploy a contract that does not exist.
  // However, the implementation can account for messages from the executor
  // arriving out of order, and a contract deployment arriving before its
  // creation.
  void deploy_contract(ContractAddress const &addr, SHA256Hash const &hash);
  void add_contract(std::shared_ptr<const Contract> contract);

  // commit or abandon the modifications made by the current block.
  void commit_block();
  void revert_block();

private:
  // Track an active contract and associated metadata.
  struct RegisteredContract {
    std::shared_ptr<const Contract> contract_;
    uint32_t deployment_count_;
  };

  void bump_contract_refcount(SHA256Hash const &hash);

  // This will be swapped out for a better implementation later.

  // Deployments and contracts that have been committed in past blocks.
  x23::MVar<std::unordered_map<ContractAddress, SHA256Hash>> deployment_map_;
  x23::MVar<std::unordered_map<SHA256Hash, RegisteredContract>> contract_map_;

  // Deployments and contracts created during the current block.
  // Entries here will overwrite those from past blocks if commit_block() is
  // called, and will be dropped by revert_block().
  x23::MVar<std::unordered_map<ContractAddress, SHA256Hash>>
      new_deployment_map_;
  x23::MVar<std::unordered_map<SHA256Hash, RegisteredContract>>
      new_contract_map_;
};

class ContractStorageServer {
public:
  using rpc_service = ContractStorageInterfaceV1;

  ContractStorageServer(ContractMap &contract_map) : contract_map_(contract_map)
  {}

  x23::Async<RPCError> commit_contract_create(Contract const &new_contract);
  x23::Async<RPCError>
  commit_contract_deploy(ContractAddress const &new_address,
                         SHA256Hash const &deployed_contract_hash);

  x23::Async<ExpectedContract> get_contract(SHA256Hash const &contract_hash);
  x23::Async<ExpectedDeployment>
  get_deployment(ContractAddress const &contract_address);

private:
  ContractMap &contract_map_;
};

static_assert(
    is_ContractStorageInterfaceV1<ContractStorageServer *, x23::Async>);

} // namespace hogd
