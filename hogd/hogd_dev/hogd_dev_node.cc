#include "hogd/hogd_dev/hogd_dev_node.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_auth.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_gateway.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/rpc_from_event.h"
#include "hogd/indexer/status.h"
#include "hogd/mempool/rpc_from_auth.h"
#include "hogd/mempool/shared.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

namespace {
constexpr char kIndexerDbSubdir[] = "indexer-db";
} // namespace

HogdDevNode::HogdDevNode(std::string bind_host,
                         std::uint16_t requested_scheduler_port)
  : bind_host_(std::move(bind_host)), event_ctx_(event_dir_.directory()),
    event_sched_server_(event_ctx_), event_exec_server_(event_ctx_),
    event_indexer_server_(event_ctx_)
{
  mempool_auth_server_->impl()->register_callback(
      [this](BlockHeight height, KeyChangeBatch key_changes) {
        return mempool_coord_server_->impl()->auth_data_changed(
            height, std::move(key_changes));
      });
  // The supervisor loads genesis over the scheduler endpoint after readiness,
  // and future dev-account genesis payloads should not trip the transport
  // default before the scheduler sees them.
  mempool_coord_server_->max_message_size_ = kDevRpcMaxMessageSize;
  mempool_auth_server_->max_message_size_ = kDevRpcMaxMessageSize;

  // Start the mempool RPC servers. All of the subsystems register with the
  // mempool, so the mempool RPC servers need to start first.
  sched_db_server_.start_server(make_bind_host_port(bind_host_, /*port=*/0));
  sched_event_server_.start_server(make_bind_host_port(bind_host_, /*port=*/0));
  sched_exec_server_.start_server(make_bind_host_port(bind_host_, /*port=*/0));
  mempool_coord_server_.start_server(
      make_bind_host_port(bind_host_, requested_scheduler_port));
  mempool_auth_server_.start_server(
      make_bind_host_port(bind_host_, /*port=*/0));

  auto sched_db_client = std::make_unique<sched_db_client_t>(
      create_client<SchedDb1>(bind_host_, sched_db_server_.port())());
  init_db_backend(std::move(sched_db_client));

  // [db_executor_server_] handles RPCs from the executor.
  db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
  db_executor_server_->start_server(
      make_bind_host_port(bind_host_, /*port=*/0));
  // [db_scheduler_server_] handles RPCs from the mempool.
  db_scheduler_server_ = std::make_unique<HogDBSchedServer>(*ctx_);
  (*db_scheduler_server_)->max_message_size_ = kDevRpcMaxMessageSize;
  db_scheduler_server_->start_server(make_bind_host_port(bind_host_,
                                                         /*port=*/0));
  db_auth_server_ = std::make_unique<HogDBAuthServer>(*ctx_);
  db_auth_server_->start_server(make_bind_host_port(bind_host_, /*port=*/0));
  // Read-only state reads served to the public RPC gateway over x23.
  db_gateway_server_ = std::make_unique<HogDBGatewayServer>(*ctx_);
  db_gateway_server_->start_server(make_bind_host_port(bind_host_, /*port=*/0));

  std::string dbauth_address =
      format_advertised_address(bind_host_, db_auth_server_->port());
  dbauth_client_t dbauth_client = make_dbauth_client(dbauth_address)();
  auth_server_ = std::make_unique<AuthServer>(std::move(dbauth_client));

  auth_db_server_->max_message_size_ = kDevRpcMaxMessageSize;
  auth_mempool_server_->max_message_size_ = kDevRpcMaxMessageSize;
  auth_db_server_.start_server(make_bind_host_port(bind_host_, /*port=*/0));
  auth_mempool_server_.start_server(make_bind_host_port(bind_host_,
                                                        /*port=*/0));
  auth_db_server_->impl()->set_auth_server(*auth_server_);
  auth_mempool_server_->impl()->set_auth_server(*auth_server_);

  event_sched_server_.max_message_size_ = kDevRpcMaxMessageSize;
  event_exec_server_.max_message_size_ = kDevRpcMaxMessageSize;
  event_indexer_server_.max_message_size_ = kDevRpcMaxMessageSize;
  event_sched_server_.start_server(make_bind_host_port(bind_host_,
                                                       /*port=*/0));
  event_exec_server_.start_server(make_bind_host_port(bind_host_,
                                                      /*port=*/0));
  event_indexer_server_.start_server(make_bind_host_port(bind_host_,
                                                         /*port=*/0));

  init_indexer_backend();

  // We start the executor server in [register_executor_with_mempool()].

  // Start the fake coordinator. The real coordinator lives in chuckd, but
  // hogd-dev only needs the scheduler back-channel that the mempool requires
  // during startup.
  coordinator_sched_server_->max_message_size_ = kDevRpcMaxMessageSize;
  coordinator_sched_server_.start_server(make_bind_host_port(bind_host_,
                                                             /*port=*/0));

  // Now have all of the subsystems register themselves with the mempool.
  register_indexer_with_event_service();
  register_db_with_mempool();
  register_event_with_mempool();
  register_auth_with_mempool();
  // The executor must register itself *after* the database and event servers
  // have registered!
  register_executor_with_mempool();
  register_coordinator_with_mempool()();
}

HogdDevNode::~HogdDevNode()
{
  try {
    // Shutdown is intentionally loud while this topology is still mirroring the
    // fragile MempoolTestInfra lifecycle; unexpected exceptions here are bugs.
    // [sched_db_server_] sends RPCs to [db_scheduler_server_] (via the
    // scheduler). Reset the scheduler pointer before destructing
    // [db_scheduler_server_] so that [sched_db_server_] does not send an RPC to
    // a stopped server.
    sched_db_server_.impl()->reset_scheduler()();
    // The scheduler sends RPCs to [db_scheduler_server_] in a detached
    // coroutine. Shut it down before destroying [db_scheduler_server_], or the
    // detached coroutine may send on an invalid file descriptor.
    mempool_coord_server_.impl()->scheduler()->shut_down()();
    // This mirrors MempoolTestInfra's current shutdown assumption: the RPC
    // servers do not yet have a coordinated clean-shutdown API, but executor
    // shutdown is required so the [Executor] destructor does not block.
    executor_.shut_down();

    db_gateway_server_.reset();
    db_auth_server_.reset();
    db_scheduler_server_.reset();
    db_executor_server_.reset();
    // [ctx_] holds a sched_db_client, so destroy it after the scheduler-facing
    // database servers are gone.
    ctx_.reset();
  } catch (const std::exception &e) {
    error("~HogdDevNode received exception: {}", e.what());
    CHECK(false);
  } catch (...) {
    error("~HogdDevNode received exception");
    CHECK(false);
  }
}

void
HogdDevNode::init_indexer_backend()
{
  std::filesystem::path db_path =
      std::filesystem::path(indexer_dir_.directory()) / kIndexerDbSubdir;
  IndexerStatus<IndexDB> db_status = IndexDB::open_or_make_new(db_path);
  if (!db_status) {
    panic("[IndexDB::open_or_make_new] failed with {}", db_status.error());
  }
  IndexDB &db = db_status.value();
  indexer_db_ = std::make_unique<IndexDB>(std::move(db));

  indexer_event_server_ =
      std::make_unique<RpcServer<IndexerEventRpc>>(*indexer_db_);
  indexer_event_server_->max_message_size_ = kDevRpcMaxMessageSize;
  indexer_event_server_->start_server(make_bind_host_port(bind_host_,
                                                          /*port=*/0));

  indexer_block_explorer_server_ =
      std::make_unique<RpcServer<IndexerBlockExplorerRpc>>(*indexer_db_);
  indexer_block_explorer_server_->max_message_size_ = kDevRpcMaxMessageSize;
  indexer_block_explorer_server_->start_server(
      make_bind_host_port(bind_host_, /*port=*/0));
}

void
HogdDevNode::register_indexer_with_event_service()
{
  auto client =
      create_client<EventIndexer1>(bind_host_, event_indexer_server_.port())();
  std::string indexer_event_address =
      format_advertised_address(bind_host_, indexer_event_server_->port());
  IndexerCard card{.protocol = version::kHogdProtocol(),
                   .address_for_event = indexer_event_address.c_str()};
  client.event_indexer_intro(card)();
}

void
HogdDevNode::init_db_backend(std::unique_ptr<sched_db_client_t> sched_db_client)
{
  DBStatus<std::uint64_t> lsn =
      Database::check(db_dir_.directory(), kRamDbBytes);
  CHECK(lsn.has_value());
  info("database '{}' passed integrity check; current LSN is {}",
       db_dir_.directory(), *lsn);

  DBStatus<std::unique_ptr<Database>> ramdb =
      Database::open_lsn(db_dir_.directory(), *lsn, kRamDbBytes);
  CHECK(ramdb.has_value());

  auto db = HogDBDAO(RAMDB(std::move(*ramdb)));
  ctx_ =
      std::make_unique<RPCContext>(std::move(db), std::move(sched_db_client));
  ctx_->initialize_cost_model(StorageCostModel::default_cost_model());
}

void
HogdDevNode::register_db_with_mempool()
{
  auto client = create_client<SchedDb1>(bind_host_, sched_db_server_.port())();
  std::string exec_address =
      format_advertised_address(bind_host_, db_executor_server_->port());
  std::string sched_address =
      format_advertised_address(bind_host_, db_scheduler_server_->port());
  std::string auth_address =
      format_advertised_address(bind_host_, db_auth_server_->port());
  client.scheddb_intro_shard({.protocol = version::kHogdProtocol(),
                              .block_height = 0,
                              .address_for_exec = exec_address.c_str(),
                              .address_for_sched = sched_address.c_str(),
                              .address_for_auth = auth_address.c_str()})();
}

void
HogdDevNode::register_event_with_mempool()
{
  auto client =
      create_client<SchedEvent1>(bind_host_, sched_event_server_.port())();
  RPCError r = client.schedevent_intro_event(
      {.protocol = version::kHogdProtocol(),
       .upid = {},
       .address_for_exec =
           format_advertised_address(bind_host_, event_exec_server_.port())
               .c_str(),
       .address_for_sched =
           format_advertised_address(bind_host_, event_sched_server_.port())
               .c_str()})();
  CHECK_EQ(r, RPCError::kRPCNone);
}

void
HogdDevNode::register_auth_with_mempool()
{
  sched_auth_client_t client =
      create_client<MempoolAuth1>(bind_host_, mempool_auth_server_.port())();
  AuthCard auth_card = {
      .protocol = version::kHogdProtocol(),
      .upid = {},
      .address_for_db =
          format_advertised_address(bind_host_, auth_db_server_.port()).c_str(),
      .address_for_mempool =
          format_advertised_address(bind_host_, auth_mempool_server_.port())
              .c_str()};
  client.mempoolauth_intro_auth(auth_card)();
  auth_server_->set_mempool_client(std::move(client));
  auth_server_->subscribe_to_auth_data_changes(auth_card)();
}

void
HogdDevNode::register_executor_with_mempool()
{
  const sockaddr_storage coordinator_sockaddr =
      host_port_to_sockaddr(bind_host_, sched_exec_server_.port());
  executor_.start(bind_host_, /*self_port=*/0, coordinator_sockaddr);
}

x23::Async<void>
HogdDevNode::register_coordinator_with_mempool()
{
  auto client = create_client<SchedulerCoord1>(bind_host_,
                                               mempool_coord_server_.port())();
  std::string coord_address =
      format_advertised_address(bind_host_, coordinator_sched_server_.port());
  CoordCard card{.protocol = version::kHogdProtocol(),
                 .address_for_sched = coord_address.c_str()};
  CHECK_EQ(co_await client.schedulercoord_intro_coord(card),
           CoordinatorRegistrationOutcome::CoordinatorRegistrationOK);
}

} // namespace hogd
