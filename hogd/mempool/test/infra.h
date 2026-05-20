#pragma once

#include <array>
#include <cassert>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <utility>

#include "address_auth.hh"
#include "built_contracts/contract_loader.h"
#include "contract.hh"
#include "db_sched.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_auth.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/hogexec/interface/executor.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/mempool/rpc_from_auth.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/shared.h"
#include "hogd/mempool/test/coordinator.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "storage_cost.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

constexpr ContractAddress
make_account_address(uint8_t i)
{
  ContractAddress
      addr{}; // Value initialization - zero-initializes all elements
  addr[0] = i;
  return addr;
}

class MempoolTestInfra {
public:
  static constexpr uint64_t kNetworkId = 0x0;
  static constexpr std::string kAddress = "127.0.0.1";
  static constexpr std::array<ContractAddress, 4> kAccountAddresses =
      std::array{make_account_address(3), make_account_address(4),
                 make_account_address(5), make_account_address(6)};

  MempoolTestInfra(bool skip_genesis_block = false,
                   size_t mempool_tx_size_limit =
                       MempoolCoordinatorRpc::kDefaultMempoolTxSizeLimit);

  ~MempoolTestInfra()
  {
    // jhumphri: I have noticed that the shutdown path is particularly prone to
    // exceptions, so I wrap the entire destructor body in a try-catch block and
    // I print the exception description followed by a [CHECK(false)]. This will
    // bring more attention to an exception when it is thrown.
    try {
      // [sched_db_server_] sends RPCs to [db_scheduler_server_] (via the
      // scheduler). We reset the scheduler pointer in [sched_db_server_] before
      // we destruct [db_scheduler_server_] below so that [sched_db_server_]
      // does not send an RPC to a stopped server and then throw an exception
      // when the RPC fails.
      sched_db_server_.impl()->reset_scheduler()();
      // The scheduler sends RPCs to [db_scheduler_server_] in a detached
      // coroutine. It is important to call [Scheduler::shut_down()] before we
      // destroy [db_scheduler_server_] so that we wait for any detached
      // coroutines to complete. Otherwise, if we destroy [db_scheduler_server_]
      // first, the detached coroutines may attempt to send RPCs on an invalid
      // file descriptor, which will cause an exception to be thrown.
      mempool_coord_server_.impl()->scheduler()->shut_down()();
      // mempool_coord_server_.shut_down();

      // TODO(jhumphri): Implement clean shutdown for all RPC servers.
      // We need to call [executor_.shut_down()] so that the [Exector]
      // destructor does not block.
      executor_.shut_down();

      // coordinator_sched_server_.shut_down();

      // event_exec_server_.shut_down();
      //  event_sched_server_.shut_down();

      db_auth_server_.reset();
      db_scheduler_server_.reset();
      db_executor_server_.reset();
      // [ctx_] holds an instance of [sched_db_client], so we need to destroy it
      // so that the [sched_db_client] is in turned destroyed and therefore does
      // not continue using the default coropoll.
      ctx_.reset();

      // sched_exec_server_.shut_down();
      // sched_event_server_.shut_down();
      // sched_db_server_.shut_down();
    } catch (const std::exception &e) {
      error("~MempoolTestInfra received exception: {}", e.what());
      CHECK(false);
    } catch (...) {
      error("~MempoolTestInfra received exception");
      CHECK(false);
    }
  }

  void set_hogs(const ContractAddress &account, int group, BlockHeight age,
                uint128_t num_hogs)
  {
    CHECK_GE(group, 0);
    CHECK_LT(group, kHogAgeNumGroups);

    HogAge average;
    memset(&average, 0, sizeof(average));
    HogDBStatus<HogAge> res = get_db().read_address_average_hog_age(account);
    if (res.has_value()) {
      average = *res;
    }

    average.address = account;
    average.average_ages[group] = age;
    average.num_hogs[group] = to_xdr(num_hogs);
    CHECK(get_db().write_address_average_hog_age({average}).has_value());
  }

  void set_address_auth_data(const ContractAddress &account,
                             const AddressAuthData &auth_data)
  {
    CHECK(get_db().write_address_auth_data(account, auth_data).has_value());
  }

  x23::Async<void> register_auth_data_updates(BlockHeight height,
                                              KeyChangeBatch key_changes)
  {
    CHECK_PTR(auth_server_.get());
    return auth_server_->register_auth_data_updates(height,
                                                    std::move(key_changes));
  }

  void set_dbtip(const DbTip &db_tip)
  {
    CHECK(get_db().write_dbtip(db_tip).has_value());
    ctx_->set_tip(db_tip);
  }

  // This getter is used by [auth_test.cc].
  MempoolCoordinatorRpc &mempool_coord_rpc()
  {
    // XXX should we return a shared_ptr?
    return *mempool_coord_server_->impl();
  }

  // This getter is used by [mempool_test.cc].
  CoordinatorSchedRpc &coord_sched_rpc()
  {
    return *coordinator_sched_server_->impl();
  }

  TestEventAggregatorSchedRpc &event_sched_rpc()
  {
    return *event_sched_server_->impl();
  }

private:
  static constexpr std::string kTestContractFileName = "fibonacci.wasm";
  static constexpr uint128_t kStorageTokenAmount = 1'000'000'000;
  static constexpr size_t kRamDbBytes = 15UL * 1024 * 1024 * 1024; // 20 GiB.

  static std::string make_address(const std::string &host, uint16_t port)
  {
    return std::format("{}:{}", host, port);
  }

  void init_db_backend(std::unique_ptr<sched_db_client_t> sched_db_client)
  {
    // check the database's integrity and find the highest valid LSN.
    DBStatus<uint64_t> lsn = Database::check(db_dir_.directory(), kRamDbBytes);
    CHECK(lsn.has_value());
    info("database '{}' passed integrity check; current LSN is {}",
         db_dir_.directory(), *lsn);

    // create the database, loading up to the highest valid LSN.
    // TODO(abelay): When sharding is supported, this will become a separate
    // step.
    DBStatus<std::unique_ptr<Database>> ramdb =
        Database::open_lsn(db_dir_.directory(), *lsn, kRamDbBytes);
    CHECK(ramdb.has_value());

    auto db = HogDBDAO(RAMDB(std::move(*ramdb)));
    ctx_ =
        std::make_unique<RPCContext>(std::move(db), std::move(sched_db_client));
    ctx_->initialize_cost_model(StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = to_xdr(int128_t{12000}),
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = to_xdr(int128_t{12000}),
        .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
    }));
  }

  HogDBDAO &get_db()
  {
    assert(ctx_);
    return ctx_->get_db();
  }

  void init_contracts()
  {
    std::shared_ptr<Contract> contract = load_contract(kTestContractFileName);

    // Write contract to hogdb.
    auto bytes = x23::serialize(*contract);
    auto contract_hash = sha256(*contract);
    CHECK(get_db()
              .write_wasm_code(contract_hash, std::as_bytes(std::span(bytes)))
              .has_value());

    for (const ContractAddress &address : kAccountAddresses) {
      // Write deployment.
      uint32_t contract_size = bytes.size();
      CHECK(
          get_db()
              .write_address_metadata(
                  address, DeployedContractHash{.hash = contract_hash,
                                                .contract_size = contract_size})
              .has_value());
      grant_initial_tokens_for_account(address);
    }
  }

  void grant_initial_tokens_for_account(const ContractAddress &address)
  {
    static_assert(kStorageTokenAmount >= 0);
    static_assert(kStorageTokenAmount <=
                  uint128_t{kHogDivisibility} * uint128_t{kTotalHogSupply});

    CHECK(get_db()
              .write_address_balance(address, kStorageTokenAmount)
              .has_value());
    // TODO(jhumphri): Multiply by something other than 10. Or just give the
    // account unlimited hogs with UINT128_MAX.
    set_hogs(address, /*group=*/kHogAgeGroupToSchedule,
             /*age=*/0, /*num_hogs=*/5'000'000'000 * 10);
  }

  void register_db_with_mempool()
  {
    auto client = create_client<SchedDb1>(kAddress, sched_db_server_.port())();
    std::string exec_address =
        make_address(kAddress, db_executor_server_->port());
    std::string sched_address =
        make_address(kAddress, db_scheduler_server_->port());
    std::string auth_address = make_address(kAddress, db_auth_server_->port());
    client.scheddb_intro_shard({.protocol = version::kHogdProtocol(),
                                .block_height = 0,
                                .address_for_exec = exec_address.c_str(),
                                .address_for_sched = sched_address.c_str(),
                                .address_for_auth = auth_address.c_str()})();
  }

  void register_event_with_mempool()
  {
    auto client = create_client<SchedEvent1>(MempoolTestInfra::kAddress,
                                             sched_event_server_.port())();
    auto r = client.schedevent_intro_event(
        {.protocol = version::kHogdProtocol(),
         .upid = {},
         .address_for_exec =
             make_address(kAddress, event_exec_server_.port()).c_str(),
         .address_for_sched =
             make_address(kAddress, event_sched_server_.port()).c_str()})();
    CHECK_EQ(r, RPCError::kRPCNone);
  }

  void register_auth_with_mempool()
  {
    sched_auth_client_t client =
        create_client<MempoolAuth1>(kAddress, mempool_auth_server_.port())();
    AuthCard auth_card = {
        .protocol = version::kHogdProtocol(),
        .upid = {},
        .address_for_db =
            make_address(kAddress, auth_db_server_.port()).c_str(),
        .address_for_mempool =
            make_address(kAddress, auth_mempool_server_.port()).c_str()};
    client.mempoolauth_intro_auth(auth_card)();
    auth_server_->set_mempool_client(std::move(client));
    auth_server_->subscribe_to_auth_data_changes(auth_card)();
  }

  void register_executor_with_mempool()
  {
    const sockaddr_storage coordinator_sockaddr =
        host_port_to_sockaddr(kAddress, sched_exec_server_.port());
    executor_.start(/*bind_ip=*/"127.0.0.1", /*self_port=*/0,
                    coordinator_sockaddr);
  }

  x23::Async<void> register_coordinator_with_mempool()
  {
    auto client =
        create_client<SchedulerCoord1>(MempoolTestInfra::kAddress,
                                       mempool_coord_server_.port())();
    std::string coord_address =
        make_address(kAddress, coordinator_sched_server_.port());
    CoordCard card{.protocol = version::kHogdProtocol(),
                   .address_for_sched = coord_address.c_str()};
    CHECK_EQ(co_await client.schedulercoord_intro_coord(card),
             CoordinatorRegistrationOutcome::CoordinatorRegistrationOK);
  }

  // Various backend components required by the database.
  TmpDir db_dir_;
  std::unique_ptr<RPCContext> ctx_;

  // These hold references to ctx_, so they should be destroyed before ctx_.
  // The database server that the executor accesses.
  std::unique_ptr<HogDBExecutorServer> db_executor_server_;
  // The database server that the scheduler accesses.
  std::unique_ptr<HogDBSchedServer> db_scheduler_server_;
  // The database server that the authenticator accesses.
  std::unique_ptr<HogDBAuthServer> db_auth_server_;

  // The event server that the mempool accesses.
  TestEventAggregator event_server_;
  TestEventAggregatorSchedServer event_sched_server_ =
      TestEventAggregatorSchedServer(event_server_);
  TestEventAggregatorExecServer event_exec_server_ =
      TestEventAggregatorExecServer(event_server_);

  // The executor server.
  Executor executor_;

  // This is a fake coordinator server that the mempool notifies when txs
  // become vetted.
  RpcServer<CoordinatorSchedRpc> coordinator_sched_server_;

  // The mempool servers.
  // We keep the [mempool_coord_server_] declaration below the various
  // database-related declarations above so that it is destructed first. We want
  // the database RPC server and its backend to be running so that the mempool
  // can flush its cache.
  RpcServer<MempoolDbRpc> sched_db_server_;
  RpcServer<MempoolCoordinatorRpc> mempool_coord_server_;
  RpcServer<MempoolAuthRpc> mempool_auth_server_;
  RpcServer<MempoolEventRpc> sched_event_server_;
  RpcServer<MempoolExecutorRpc> sched_exec_server_;

  std::unique_ptr<AuthServer> auth_server_;
  RpcServer<AuthDBRpc> auth_db_server_;
  RpcServer<AuthMempoolRpc> auth_mempool_server_;
};

} // namespace hogd
