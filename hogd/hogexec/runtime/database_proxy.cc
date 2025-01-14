#include "hogd/hogexec/runtime/database_proxy.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {

DatabaseProxy::DatabaseProxy(x23::CoroPoll &cp, std::string address,
                             std::string port)
  : db_client_([&] {
      auto fd = cp.tcpconnect(address, port)();
      if (!fd) {
        exit(-1);
      }

      auto client_config = x23::rpc_stream_logged(cp, *fd);
      return client_config.template client<DatabaseToExecutorInterfaceV1>();
    }())
{}

bool
DatabaseProxy::try_reserve_delta(BlockID block_id, TransactionID tx_id,
                                 AddressAndKey const &addrkey,
                                 StorageDelta const &delta)
{
  auto res = db_client_.try_reserve_delta(block_id, tx_id, addrkey, delta)();

  if (res == RPCError::kNone) {
    return true;
  }
  if (res == RPCError::kStorageWriteFailed) {
    return false;
  }
  // This function could also return an expected, but somehow
  // this needs to get translated to a kUnrecoverable error.
  throw std::runtime_error("unhandleable error");
}

void
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id)
{
  db_client_.commit_transaction(block_id, tx_id)();
}

void
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  db_client_.abort_transaction(block_id, tx_id)();
}

std::optional<StorageItem>
DatabaseProxy::get_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = db_client_.get_item(block_id, addrkey)();

  if (res.error() == RPCError::kNone) {
    return res.item();
  }
  if (res.error() == RPCError::kNoStorageItem) {
    return std::nullopt;
  }
  throw std::runtime_error("unhandleable error");
}

void
DatabaseProxy::commit_contract_create(BlockID block_id,
                                      Contract const &contract)
{
  db_client_.commit_contract_create(block_id, contract)();
}

void
DatabaseProxy::commit_contract_deploy(BlockID block_id,
                                      ContractAddress const &addr,
                                      SHA256Hash const &contract_hash)
{
  db_client_.commit_contract_deploy(block_id, addr, contract_hash)();
}

std::shared_ptr<const Contract>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = db_client_.get_contract(block_id, contract_hash)();

  if (res.error() == RPCError::kNone) {
    return std::make_shared<const Contract>(res.contract());
  }
  if (res.error() == RPCError::kContractNExist) {
    return nullptr;
  }
  throw std::runtime_error("unhandleable error");
}

std::optional<SHA256Hash>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = db_client_.get_deployment(block_id, addr)();

  if (res.error() == RPCError::kNone) {
    return res.deployed_hash();
  }
  if (res.error() == RPCError::kNoContractDeployed) {
    return std::nullopt;
  }
  throw std::runtime_error("unhandleable error");
}

} // namespace hogd
