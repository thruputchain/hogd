#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_auth.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/hog_db/rpc_from_sched.h"
#include "hogd/hog_exec/runtime/test_util/test_sched_db_server.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "network_config.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/rpc_common.h"

namespace hogd {

class HogdbWrapper {
public:
  // [heap_bytes] sizes the ramdb heap. The default suits a test seeding a
  // handful of items; a caller seeding enough of them to measure a lookup has
  // to say so, because the allocator fails the write rather than growing.
  explicit HogdbWrapper(size_t heap_bytes = kDefaultHeapSize)
    : ctx_(HogDbDAO(open_db(heap_bytes)), make_sched_db_client())
  {
    db_sched_server_.start_server(/*port=*/0);
    db_executor_server_.start_server(/*port=*/0);
    db_auth_server_.start_server(/*port=*/0);
  }

  uint16_t exec_port() { return db_executor_server_.port(); }
  uint16_t coord_port() { return db_sched_server_.port(); }
  uint16_t auth_port() { return db_auth_server_.port(); }

  // Make [block_id] readable: publish the tip whose child it is, and open a
  // BlockTX for it.
  //
  // Both halves are needed and neither is what BlockSnapshotServer does, which
  // is why a harness cannot simply repoint at this class. A fresh database has
  // no committed tip, and the executor reads its NetworkConfig from the tip,
  // so without one every invoke faults on a zero cross-invoke depth. And a
  // real database refuses to serve a read for a block it has no BlockTX for --
  // is_snapshot_read_stale answers true for an id it does not know -- where
  // the light server has no notion of an open block at all.
  //
  // The tip is the parent's, so its id is block_id - 1; that same id is the
  // parent the BlockTX opens against.
  void prepare_block(BlockID block_id, BlockHeight tip_block_height,
                     NetworkConfig network_config, BlockExecutionMode mode);

  void grant_storage_token(ContractAddress const &address, int128_t amount);

  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> const &item);

  void write_contract(ContractAddress const &addr,
                      std::shared_ptr<const Contract> const &contract);
  void write_deployment(ContractAddress const &addr,
                        DeployedContractHash const &deployment,
                        StorageCostModel const *cost_model);

  std::map<ContractAddress, int128_t> get_hogs_transferred(BlockHeight height)
  {
    test_sched_db_server_->impl()->wait_for_hog_transferred_height(height);
    return test_sched_db_server_->impl()->get_hogs_transferred(height);
  }

  auto &get_db() { return ctx_.get_db(); }

  StorageCostModel get_cost_model()
  {
    return get_db().read_cost_model().value();
  }

private:
  constexpr static size_t kDefaultHeapSize = 10 * 1024 * 1024;
  static HogDbDAO open_db(size_t heap_bytes);
  using coord_client_t =
      DbSched1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;
  coord_client_t make_coord_client();
  std::unique_ptr<SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
  make_sched_db_client();

  RpcServer<TestSchedDBServer> test_sched_db_server_;
  std::optional<coord_client_t> coord_client_;
  RPCContext ctx_;
  HogDbExecutorServer db_executor_server_ = HogDbExecutorServer(ctx_);
  HogDbSchedServer db_sched_server_ = HogDbSchedServer(ctx_);
  HogDbAuthServer db_auth_server_ = HogDbAuthServer(ctx_);
};
} // namespace hogd
