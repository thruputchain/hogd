#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "genesis.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/db_shard_routing/sharding.h" // kDefaultShard
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/fake_coordinator.h"
#include "hogd/devnode/hog_devnode_tmp_dir.h"
#include "hogd/hog_auth/auth_server.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_auth.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/hog_db/rpc_from_gateway.h"
#include "hogd/hog_db/rpc_from_sched.h"
#include "hogd/hog_event/rpc_context.h"
#include "hogd/hog_event/rpc_from_exec.h"
#include "hogd/hog_event/rpc_from_indexer.h"
#include "hogd/hog_event/rpc_from_sched.h"
#include "hogd/hog_exec/interface/executor.h"
#include "hogd/hog_exec/interface/rpc_from_view_gateway.h"
#include "hogd/hog_indexer/db.h"
#include "hogd/hog_indexer/pull_controller.h"
#include "hogd/hog_indexer/recent_tx_failure_cache.h"
#include "hogd/hog_indexer/rpc_from_block_explorer.h"
#include "hogd/hog_mempool/rpc_from_auth.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/rpc_from_db.h"
#include "hogd/hog_mempool/rpc_from_event.h"
#include "hogd/hog_mempool/rpc_from_exec.h"
#include "hogd/hog_mempool/shared.h"
#include "transaction.hh"
#include "types.hh" // ContractAddress
#include "x23/async.h"

namespace hogd {

class HogDevnodeNode {
public:
  // RAMDB maps this capacity but does not consume it as steady-state RSS.
  static constexpr std::size_t kDefaultRamDbBytes = 15UL * 1024 * 1024 * 1024;

  HogDevnodeNode(
      std::string bind_host, std::uint16_t requested_scheduler_port,
      LocalBootstrapConfig const &bootstrap_config = {},
      std::optional<std::filesystem::path> const &data_dir = std::nullopt,
      NetworkHash network_hash = {},
      std::size_t ramdb_bytes = kDefaultRamDbBytes,
      std::size_t num_db_shards = 1);
  ~HogDevnodeNode();

  HogDevnodeNode(HogDevnodeNode const &) = delete;
  HogDevnodeNode &operator=(HogDevnodeNode const &) = delete;
  HogDevnodeNode(HogDevnodeNode &&) = delete;
  HogDevnodeNode &operator=(HogDevnodeNode &&) = delete;

  std::string const &bind_host() const { return bind_host_; }
  // The supervisor calls this the scheduler endpoint; hogd exposes it through
  // the coordinator-facing SchedulerCoord1 RPC server.
  std::uint16_t scheduler_port() const { return mempool_coord_server_.port(); }
  // Advertised in the runtime manifest as the `indexer` endpoint.
  std::uint16_t read_port() const
  {
    return indexer_block_explorer_server_->port();
  }
  // Advertised in the runtime manifest as the `dbGateway` endpoint. One server
  // for every shard: it splits each batch to the owning shards, so the manifest
  // keeps a single dbGateway endpoint.
  std::uint16_t gateway_port() const { return db_gateway_server_->port(); }
  std::size_t num_db_shards() const { return num_db_shards_; }
  // Advertised in the runtime manifest as the `viewGateway` endpoint.
  std::uint16_t view_gateway_port() const
  {
    return view_gateway_server_->port();
  }
  std::vector<BootstrapAccountMetadata> const &bootstrap_accounts() const
  {
    return bootstrap_accounts_;
  }
  LocalBootstrapConfig const &bootstrap_config() const
  {
    return bootstrap_config_;
  }
  GenesisConfiguration const &genesis_configuration() const
  {
    return genesis_configuration_;
  }
  GenesisConfigurationV2 const &genesis_configuration_v2() const
  {
    return genesis_configuration_v2_;
  }
  bool recovered_existing_chain() const noexcept
  {
    return recovered_existing_chain_;
  }
  std::string const &runtime_dir() const { return runtime_dir_.directory(); }

private:
  // ONC RPC single-fragment record limit; x23 cannot frame a larger message.
  static constexpr std::size_t kDevRpcMaxMessageSize = 0x7fffffff;

  // One db shard: its ramdb directory, RPC context, and the per-shard servers
  // hogdb exposes (the gateway spans shards, so it lives on the node).
  // Declaration order is the teardown order: [dir] outlives the ramdb mapped
  // out of it, and the servers borrow [ctx] so they go first.
  struct DbShard {
    DbShard(std::optional<std::filesystem::path> const &data_dir,
            std::string_view subdir)
      : dir(data_dir, subdir)
    {}

    HogDevnodeTmpDir dir;
    std::unique_ptr<RPCContext> ctx;
    std::unique_ptr<HogDbExecutorServer> executor_server;
    std::unique_ptr<HogDbSchedServer> scheduler_server;
    std::unique_ptr<HogDbAuthServer> auth_server;
  };

  // Every shard's context in shard-id order, for the servers that span shards.
  std::vector<RPCContext *> shard_contexts();

  HogDbDAO open_db_backend(std::size_t shard_id) const;
  void init_db_backend(std::size_t shard_id, HogDbDAO db,
                       std::unique_ptr<sched_db_client_t> sched_db_client);
  // The shard that owns [address], per shard_of_address. Reads that skip this
  // ask a shard that does not hold the item.
  RPCContext &db_for(ContractAddress const &address);
  // kDefaultShard, for the requests that carry no address: every shard
  // replicates the '<' globals (db tip, cost model), so any of them answers.
  // Const like the unique_ptr member it replaced -- a const node still hands
  // out a mutable context.
  RPCContext &default_shard_db() const
  {
    return *db_shards_[kDefaultShard]->ctx;
  }
  void init_indexer_backend();
  void start_indexer_servers();
  void reconcile_event_outbox_with_recovered_tip();
  void validate_event_outbox_against_recovered_tip() const;
  void reconcile_indexer_with_recovered_tip();
  void start_indexer_pull();
  void register_db_with_mempool();
  void register_event_with_mempool();
  AuthCard register_auth_with_mempool();
  void register_executor_with_mempool();
  x23::Async<void> register_coordinator_with_mempool();
  LocalBootstrapConfig
  validate_fresh_data_dir(LocalBootstrapConfig const &requested_config) const;
  LocalBootstrapConfig
  validate_recovered_data_dir(LocalBootstrapConfig const &requested_config);
  void write_data_dir_metadata() const;
  void load_recovered_bootstrap_metadata(LocalBootstrapConfig const &config);
  x23::Async<void>
  materialize_bootstrap_accounts(LocalBootstrapConfig const &config);
  x23::Async<void> initialize_fresh_event_outbox_origin();
  void commit_initialization_batch(SignedTransactionBatch const &txs);
  void refresh_bootstrap_account_balances();
  void prepare_for_failed_construction() noexcept;

  std::string bind_host_;
  const NetworkHash network_hash_;
  std::optional<std::filesystem::path> data_dir_;
  LocalBootstrapConfig bootstrap_config_;
  // Total RAMDB capacity across every shard; each shard reserves an equal
  // slice.
  const std::size_t ramdb_bytes_;
  const std::size_t num_db_shards_;
  GenesisConfiguration genesis_configuration_;
  GenesisConfigurationV2 genesis_configuration_v2_;
  std::vector<BootstrapAccountMetadata> bootstrap_accounts_;
  bool recovered_existing_chain_ = false;

  HogDevnodeTmpDir runtime_dir_;
  // Declaration order matters: event_ctx_ borrows event_dir_.directory(),
  // and the event RPC servers below borrow event_ctx_.
  HogDevnodeTmpDir event_dir_;
  HogDevnodeTmpDir indexer_dir_;
  std::unique_ptr<IndexDB> indexer_db_;
  // Declaration order matters: the indexer services below borrow
  // indexer_failure_cache_ by reference, so it must be declared before (and
  // thus outlive) them.
  RecentTxFailureCache indexer_failure_cache_;
  std::unique_ptr<RpcServer<IndexerBlockExplorerRpc>>
      indexer_block_explorer_server_;
  // Index is the shard id, so [db_shards_[i]] is shard i of
  // [db_shards_.size()]. Held behind pointers because HogDevnodeTmpDir is
  // neither copyable nor movable.
  std::vector<std::unique_ptr<DbShard>> db_shards_;
  EventRPCContext event_ctx_;

  // Both span every shard, so they are declared after [db_shards_] and torn
  // down before it.
  std::unique_ptr<HogDbGatewayServer> db_gateway_server_;
  std::unique_ptr<ViewGatewayServer> view_gateway_server_;

  EventMempoolServer event_sched_server_;
  EventExecServer event_exec_server_;
  // Declaration order keeps the pull endpoint and its borrowed state alive
  // until the controller has stopped and joined.
  EventIndexerServer event_indexer_server_;
  std::unique_ptr<IndexerPullController> indexer_pull_controller_;

  Executor executor_;

  RpcServer<DevCoordinatorSchedRpc> coordinator_sched_server_;

  // [mempool_auth_server_] invokes [MempoolCoordinatorRpc::auth_data_changed]
  // via the [this]-capturing callback the coordinator registers in
  // [finish_init()], and that callback broadcasts on the coordinator's
  // [auth_data_height_]. The coordinator is constructed from
  // [mempool_auth_server_->impl()], so it must be declared first and would
  // otherwise be destroyed last -- after the coordinator -- letting an
  // in-flight callback race the coordinator's destructor. Hold it behind a
  // pointer so the failed-construction cleanup can drain it (via [~RpcServer],
  // which waits for in-flight handlers) before the coordinator is torn down.
  // See [prepare_for_failed_construction]. (The normal ~HogDevnodeNode path
  // instead drains via the DB subscription manager in [ctx_.reset()].)
  std::unique_ptr<RpcServer<MempoolAuthRpc>> mempool_auth_server_ =
      std::make_unique<RpcServer<MempoolAuthRpc>>();
  RpcServer<MempoolDbRpc> sched_db_server_;
  RpcServer<MempoolCoordinatorRpc> mempool_coord_server_ =
      RpcServer<MempoolCoordinatorRpc>(
          sched_db_server_->impl(), mempool_auth_server_->impl(), network_hash_,
          MempoolCoordinatorRpc::kDefaultMempoolTxSizeLimit);
  RpcServer<MempoolEventRpc> sched_event_server_;
  RpcServer<MempoolExecutorRpc> sched_exec_server_;

  std::unique_ptr<AuthServer> auth_server_;
  RpcServer<AuthDBRpc> auth_db_server_;
  RpcServer<AuthMempoolRpc> auth_mempool_server_;
};

} // namespace hogd
