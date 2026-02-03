#include "hogd/test_ffi/db_context_wrapper.h"

#include <cassert>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "auth_rpc.hh"
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/rpc_client.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {
dbauth_client_t
make_dbauth_client(x23::HostPort const &hostport)
{
  return make_rpc_client<DBAuth1>(hostport)();
}
} // namespace

DBContext::DBContext() : db_proxy_(nullptr), snapshot_cache_(nullptr)
{
  db_proxy_ = std::make_unique<DatabaseProxy>(
      LocalMockDB{.read_port_number_ = hogdb_wrapper_.exec_port()});
  event_server_proxy_ = std::make_unique<EventServerProxy>(
      x23::HostPort{"localhost", event_exec_server_.start_server(0)});

  db_sched_client_ = make_srpc_client<DbSched1>(
      x23::HostPort{"localhost", hogdb_wrapper_.coord_port()}, false);

  x23::HostPort auth_port{"localhost", hogdb_wrapper_.auth_port()};
  auth_server_ =
      std::make_unique<AuthServer>(make_dbauth_client(auth_port), 16);
  auth_rpc_servers_ = std::make_unique<AuthRpcServers>(*auth_server_);
  auth_rpc_servers_->start_servers(0, 0);

  auth_server_->subscribe_to_auth_data_changes(
      auth_rpc_servers_->make_auth_card("localhost"))();

  auto res =
      db_sched_client_.dbsched_load_genesis_configuration(GenesisConfiguration{
          .initial_config = {},
          .storage_cost_model = get_default_cost_model_xdr(),
      });
  if (res != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error(
        "DBContext: dbsched_load_genesis_configuration failed");
  }

  advance_new_block();
}

void
DBContext::advance_new_block(BlockExecutionMode mode)
{
  if (current_block_id_ > 0) {
    auto res = db_sched_client_.dbsched_close_snapshot(current_block_id_);
    if (res.result() != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error(
          "advance_new_block: dbsched_close_snapshot failed");
    }

    res = db_sched_client_.dbsched_commit_snapshot(current_block_id_);
    if (res.result() != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error(
          "advance_new_block: dbsched_commit_block failed");
    }

    event_server_.close_block(current_block_id_, current_block_id_);
  }

  auto res =
      db_sched_client_.dbsched_create_snapshot(current_block_id_,
                                               current_block_id_ + 1, mode);
  if (res != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("advance_new_block: dbsched_open_snapshot failed");
  }

  current_block_id_++;
  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(current_block_id_, *db_proxy_);
  event_server_.open_block(current_block_id_);
}

} // namespace hogd
