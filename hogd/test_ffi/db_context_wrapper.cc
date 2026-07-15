#include "hogd/test_ffi/db_context_wrapper.h"

#include <cassert>
#include <cstdlib>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "types.hh"
#include "x23/cpumap.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {
dbauth_client_t
make_dbauth_client(x23::HostPort const &hostport)
{
  auto client = make_rpc_client<DBAuth1>(hostport)();
  if (!client)
    panic("make_dbauth_client failed: {}",
          client.error().error_code().message());
  return std::move(*client);
}
} // namespace

DBContext::DBContext(StorageCostModelXdr const &cost_model)
  : db_proxy_(nullptr), snapshot_cache_(nullptr)
{
  db_proxy_ = std::make_unique<DatabaseProxy>(
      LocalMockDB{.read_port_number_ = hogdb_wrapper_.exec_port()});
  event_server_proxy_ = std::make_unique<EventServerProxy>(
      x23::HostPort{"localhost", event_exec_server_.start_server(0)},
      *x23::single_bucket_map);

  db_sched_client_ = make_srpc_client<DbSched1>(
      x23::HostPort{"localhost", hogdb_wrapper_.coord_port()}, false);

  x23::HostPort auth_port{"localhost", hogdb_wrapper_.auth_port()};
  auth_server_ =
      std::make_unique<AuthServer>(make_dbauth_client(auth_port), 16);
  auth_db_server_->max_message_size_ = hogd::LoadgenRpc::kRpcMaxMessageSize;
  auth_db_server_->impl()->set_auth_server(*auth_server_);
  auth_db_server_.start_server(/*port=*/0);
  {
    std::string address_for_db =
        std::format("localhost:{}", auth_db_server_.port());
    std::string address_for_mempool = std::format("INVALID");
    AuthCard auth_card = {.protocol = version::kHogdProtocol(),
                          .upid = {},
                          .address_for_db = address_for_db.c_str(),
                          .address_for_mempool = address_for_mempool.c_str()};
    auth_server_->subscribe_to_auth_data_changes(auth_card)();
  }

  GenesisConfiguration genesis_configuration{.initial_config = {},
                                             .storage_cost_model = cost_model};
  uint64_t network_id = 0x0;
  SHA256Hash txset_hash{};
  SHA256Hash events_hash{};
  auto res = db_sched_client_.dbsched_load_genesis_configuration(
      genesis_configuration, network_id, txset_hash, events_hash);
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

    res = db_sched_client_.dbsched_commit_snapshot(current_block_id_,
                                                   /*block_hash=*/{},
                                                   /*events_hash=*/{});
    if (res.result() != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error(
          "advance_new_block: dbsched_commit_block failed");
    }

    event_server_.close_block(current_block_id_, current_block_id_)();
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
  event_server_.open_block(current_block_id_)();
}

std::optional<WireStorageItem>
DBContext::get_snapshot_item(AddressAndKey const &addrkey)
{
  auto res = db_proxy_->get_snapshot_item(current_block_id_, addrkey);
  if (!res) {
    throw std::runtime_error("get_snapshot_item failed");
  }

  return res.value();
}

} // namespace hogd
