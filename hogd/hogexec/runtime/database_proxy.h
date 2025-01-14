#pragma once

#include <memory>
#include <optional>
#include <string>

#include "rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {

// Abstract away database sharding from the rest of the executor.
class DatabaseProxy {
public:
  // TODO(geoff): swap in other connection details, once sharding is worked out
  DatabaseProxy(x23::CoroPoll &cp, std::string address, std::string port);

  DatabaseProxy(DatabaseProxy const &) = delete;
  DatabaseProxy &operator=(DatabaseProxy const &) = delete;
  DatabaseProxy(DatabaseProxy &&) = delete;
  DatabaseProxy &operator=(DatabaseProxy &&) = delete;

  bool try_reserve_delta(BlockID block_id, TransactionID tx_id,
                         AddressAndKey const &addrkey,
                         StorageDelta const &delta);

  void commit_transaction(BlockID block_id, TransactionID tx_id);
  void abort_transaction(BlockID block_id, TransactionID tx_id);

  std::optional<StorageItem> get_item(BlockID block_id,
                                      AddressAndKey const &addrkey);

  void commit_contract_create(BlockID block_id, Contract const &contract);

  void commit_contract_deploy(BlockID block_id, ContractAddress const &addr,
                              SHA256Hash const &contract_hash);

  std::shared_ptr<const Contract> get_contract(BlockID block_id,
                                               SHA256Hash const &contract_hash);
  std::optional<SHA256Hash> get_deployment(BlockID block_id,
                                           ContractAddress const &addr);

private:
  using client_t = DatabaseToExecutorInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  client_t db_client_;
};

} // namespace hogd
