#pragma once

#include <map>
#include <memory>

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
#include "sched_db.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/rpc_common.h"

namespace hogd {

class HogdbWrapper {
public:
  HogdbWrapper() : ctx_(HogDbDAO(open_db()), make_sched_db_client())
  {
    db_sched_server_.start_server(/*port=*/0);
    db_executor_server_.start_server(/*port=*/0);
    db_auth_server_.start_server(/*port=*/0);
  }

  uint16_t exec_port() { return db_executor_server_.port(); }
  uint16_t coord_port() { return db_sched_server_.port(); }
  uint16_t auth_port() { return db_auth_server_.port(); }

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
  static HogDbDAO open_db();
  std::unique_ptr<SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
  make_sched_db_client();

  RpcServer<TestSchedDBServer> test_sched_db_server_;
  RPCContext ctx_;
  HogDbExecutorServer db_executor_server_ = HogDbExecutorServer(ctx_);
  HogDbSchedServer db_sched_server_ = HogDbSchedServer(ctx_);
  HogDbAuthServer db_auth_server_ = HogDbAuthServer(ctx_);
};
} // namespace hogd
