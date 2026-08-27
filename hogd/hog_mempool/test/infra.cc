#include "hogd/hog_mempool/test/infra.h"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "consensus.hh"
#include "db_sched.hh"
#include "header.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_auth/auth_server.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "hogd/hog_db/rpc_from_auth.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/hog_db/rpc_from_sched.h"
#include "hogd/hog_exec/shared.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "transaction.hh"

namespace hogd {

MempoolTestInfra::MempoolTestInfra(const MempoolTestInfraOptions &options)
  : owned_db_dir_(options.db_directory.has_value()
                      ? nullptr
                      : std::make_unique<TmpDir>()),
    db_directory_(owned_db_dir_ ? owned_db_dir_->directory()
                                : *options.db_directory),
    mempool_coord_server_(RpcServer<MempoolCoordinatorRpc>(
        sched_db_server_->impl(), mempool_auth_server_->impl(), kNetworkHash,
        options.mempool_tx_size_limit))
{
  // Multiple tests run within the same process, so we need to clear the
  // registration and metrics at the beginning of each test.
  Registration::registration().clear();
  Registration::registration().set_network_hash(kNetworkHash);
  Metrics::instance().clear();
  init_hog_exec_metrics();

  mempool_auth_server_->max_message_size_ = kBulkRpcMaxMessageSize;

  // Start the mempool RPC servers. All of the subsystems register with the
  // mempool, so the mempool RPC servers need to start first.
  // HogDb sends block-wide HogsTransferred with the bulk cap, so the DB-facing
  // mempool receiver must match it (mirrors mempool_init in hog_mempool.cc).
  sched_db_server_->max_message_size_ = kBulkRpcMaxMessageSize;
  sched_db_server_.start_server(/*port=*/0);
  sched_event_server_.start_server(/*port=*/0);
  sched_exec_server_.start_server(/*port=*/0);
  mempool_coord_server_.start_server(/*port=*/0);
  mempool_auth_server_.start_server(/*port=*/0);

  auto sched_db_client = std::make_unique<sched_db_client_t>(
      create_client<SchedDb1>(kAddress, sched_db_server_.port())().value());
  // Initialize backend and blockchain state for tests.
  init_db_backend(std::move(sched_db_client));
  ConsensusValue initial_value{};
  initial_value.proposal_header.block_height = -1;
  Registration::registration().publish_event_canonical_tip(
      EventCanonicalTip{.block_id = std::numeric_limits<BlockID>::max(),
                        .consensus_value = std::move(initial_value)});
  if (!options.recover_existing_chain) {
    init_contracts();
  }

  // Start the database RPC servers.
  // [db_executor_server_] handles RPCs from the executor.
  db_executor_server_ = std::make_unique<HogDbExecutorServer>(*ctx_);
  db_executor_server_->start_server(/*port=*/0);
  // [db_scheduler_server_] handles RPCs from the mempool.
  db_scheduler_server_ = std::make_unique<HogDbSchedServer>(*ctx_);
  (*db_scheduler_server_)->max_message_size_ = kBulkRpcMaxMessageSize;
  db_scheduler_server_->start_server(/*port=*/0);
  db_auth_server_ = std::make_unique<HogDbAuthServer>(*ctx_);
  db_auth_server_->start_server(/*port=*/0);

  // Start the auth servers.
  std::string dbauth_address = make_address(kAddress, db_auth_server_->port());
  dbauth_client_t dbauth_client = make_dbauth_client(dbauth_address)().value();
  auth_server_ =
      std::make_unique<AuthServer>(std::move(dbauth_client), kNetworkHash);

  auth_db_server_->max_message_size_ = kBulkRpcMaxMessageSize;
  auth_mempool_server_->max_message_size_ = kBulkRpcMaxMessageSize;
  auth_db_server_.start_server(/*port=*/0);
  auth_mempool_server_.start_server(/*port=*/0);
  auth_db_server_->impl()->set_auth_server(*auth_server_);
  auth_mempool_server_->impl()->set_auth_server(*auth_server_);

  // Start the event RPC servers.
  event_sched_server_->start_server(/*port=*/0);
  event_exec_server_.start_server(/*port=*/0);

  // We start the executor server in [register_executor_with_mempool()].

  // Start the fake coordinator. We need a coordinator to register itself with
  // the mempool. The real coordinator implementation is in the chuckd repo
  // and is written in go. So to ease test implementation, we use a fake
  // coordinator.
  coordinator_sched_server_->max_message_size_ = kBulkRpcMaxMessageSize;
  coordinator_sched_server_.start_server(/*port=*/0);

  // Now have all of the subsystems register themselves with the mempool.
  register_db_with_mempool();
  register_auth_with_mempool();
  if (options.auth_startup_order ==
      AuthStartupOrder::kSubscribeBeforeCoordinator) {
    subscribe_auth_to_data_changes();
  }
  register_coordinator_with_mempool()();
  if (options.auth_startup_order ==
      AuthStartupOrder::kSubscribeAfterCoordinator) {
    subscribe_auth_to_data_changes();
  }

  if (!options.skip_genesis_block) {
    const StorageCostModel cost_model = StorageCostModel::default_cost_model();
    GenesisConfigurationBuilder builder;
    auto root_contract = load_stock_genesis_root_contract();
    CHECK_EQ(
        mempool_coord_server_->impl()->schedcoord_load_genesis_configuration(
            builder.build(kNetworkHash, cost_model, *root_contract))(),
        DbSchedResult::DbSchedResultOK);
    register_event_with_mempool();
    register_executor_with_mempool();
    mempool_coord_server_->impl()->finish_runtime_init()();

    const SchedulerGetTipResult parent =
        mempool_coord_server_->impl()->schedcoord_get_tip()();
    CHECK_EQ(parent.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    SignedTransactionBatch ceremony_txs;
    ceremony_txs.append_range(
        builder.make_genesis_ceremony_transactions(cost_model));
    GenesisCeremonyRequest request{
        .expected_parent_height = 0,
        .expected_parent_value_hash = sha256(parent.db_tip().value),
        .context = ProposalContext{.protocol = 0},
        .transactions = t_to_parallel_batch(ceremony_txs)(),
    };
    CHECK_EQ(mempool_coord_server_->impl()
                 ->schedulercoord_commit_genesis_ceremony(std::move(request))()
                 .outcome(),
             GenesisCeremonyOutcome::GenesisCeremonyCommitted);
  }
}

} // namespace hogd
