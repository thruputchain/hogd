#include "hogd/test_ffi/mock_db_server.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "db_read_interface.hh"
#include "hogd/base/xdr_util.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/fileio.h"
#include "x23/rpc.h"

namespace hogd {

x23::Async<void>
MockDBServer::db_snapshot_read_null()
{
  co_return;
}

x23::Async<ExpectedStorageItem>
MockDBServer::get_snapshot_item(BlockID block, AddressAndKey const &addrkey)
{
  if (block != block_id_) {
    co_return ExpectedStorageItem(RPCError::kBadBlockID);
  }

  auto m = items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    co_return ExpectedStorageItem(RPCError::kNoStorageItem);
  }
  ExpectedStorageItem out(RPCError::kNone);
  out.item() = it->second;
  co_return out;
}
x23::Async<ExpectedContract>
MockDBServer::get_contract(BlockID block, SHA256Hash const &contract_hash)
{
  if (block != block_id_) {
    co_return ExpectedContract(RPCError::kBadBlockID);
  }

  auto m = contracts_.take();
  auto it = m->find(contract_hash);
  if (it == m->end()) {
    co_return ExpectedContract(RPCError::kContractNotExist);
  }
  ExpectedContract out(RPCError::kNone);
  out.contract() = it->second;
  co_return out;
}

x23::Async<ExpectedDeployment>
MockDBServer::get_deployment(BlockID block,
                             ContractAddress const &contract_address)
{
  if (block != block_id_) {
    co_return ExpectedDeployment(RPCError::kBadBlockID);
  }

  auto m = deployments_.take();
  auto it = m->find(contract_address);
  if (it == m->end()) {
    co_return ExpectedDeployment(RPCError::kNoContractDeployed);
  }
  ExpectedDeployment out(RPCError::kNone);
  out.deployed_hash() = it->second;
  co_return out;
}

void
MockDBServer::start_server(uint32_t port_number)
{
  auto lfd = x23::tcplisten(port_number);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  [](x23::CoroPoll &cp, MockDBServer *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(server);
    }
  }(cp_, this, std::move(lfd));
}

} // namespace hogd
