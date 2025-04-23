
#pragma once

#include <unordered_map>

#include "contract.hh"
#include "db_read_interface.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/mvar.h"
#include "x23/threadpool.h"

namespace hogd {

// Serve rpcs for runtime tests.
//
// Initialize manually with database snapshot state.
//
// Eventually, once the DB is merged in, this will get replaced
// by a full mock database.  But that code is pending @abelay merging
// things.
class MockDBServer {
public:
  using rpc_service = DatabaseSnapshotReadInterfaceV1;

  MockDBServer(BlockID block_id) : block_id_(block_id) {}

  static x23::Async<void> db_snapshot_read_null();

  x23::Async<ExpectedStorageItem>
  get_snapshot_item(BlockID block, AddressAndKey const &addrkey);
  x23::Async<ExpectedContract> get_contract(BlockID block,
                                            SHA256Hash const &contract_hash);
  x23::Async<ExpectedDeployment>
  get_deployment(BlockID block, ContractAddress const &contract_address);

  void start_server(uint32_t port_number);

private:
  BlockID block_id_;

  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  x23::MVar<std::unordered_map<AddressAndKey, StorageItem>> items_;
  x23::MVar<std::unordered_map<SHA256Hash, Contract>> contracts_;
  x23::MVar<std::unordered_map<ContractAddress, SHA256Hash>> deployments_;
};

static_assert(is_DatabaseSnapshotReadInterfaceV1<MockDBServer *, x23::Async>);

} // namespace hogd
