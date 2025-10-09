#pragma once

#include <array>
#include <cstring>
#include <format>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "account.hh"
#include "built_contracts/fibonacci.h"
#include "contract.hh"
#include "contract_loader.h"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
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
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/hogexec/interface/executor.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/test/coordinator.h"
#include "hogd/mempool/types.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "storage_cost.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

constexpr size_t kNumTxs = 100;

class MempoolTestInfra {
public:
  static constexpr std::string kAddress = "127.0.0.1";
  static constexpr auto kAccountAddresses = [] {
    constexpr auto kMake = NativeContractTestFramework::make_contract_address;
    return std::array{kMake(3), kMake(4), kMake(5), kMake(6)};
  }();

  MempoolTestInfra()
  {
    // Multiple tests run within the same process, so we need to clear the
    // registration at the beginning of each test.
    Registration::registration().clear();

    // Start the mempool RPC servers. All of the subsystems register with the
    // mempool, so the mempool RPC servers need to start first.
    sched_db_server_.start_server(/*port=*/0);
    sched_event_server_.start_server(/*port=*/0);
    sched_exec_server_.start_server(/*port=*/0);
    mempool_coord_server_.start_server(/*port=*/0);

    // Initialize backend and blockchain state for tests.
    init_db_backend();
    init_contracts();

    // Start the database RPC servers.
    // [db_executor_server_] handles RPCs from the executor.
    db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
    db_executor_server_->start_server(/*port=*/0);
    // [db_scheduler_server_] handles RPCs from the mempool.
    db_scheduler_server_ = std::make_unique<HogDBSchedServer>(*ctx_);
    db_scheduler_server_->start_server(/*port=*/0);

    // Start the event RPC servers.
    event_sched_server_.start_server(/*port=*/0);
    event_exec_server_.start_server(/*port=*/0);

    // We start the executor server in [register_executor_with_mempool()].

    // Start the fake coordinator. We need a coordinator to register itself with
    // the mempool. The real coordinator implementation is in the chuckd repo
    // and is written in go. So to ease test implementation, we use a fake
    // coordinator.
    coordinator_sched_server_.start_server(/*port=*/0);

    // Now have all of the subsystems register themselves with the mempool.
    register_db_with_mempool();
    register_event_with_mempool();
    // The executor must register itself *after* the database and event servers
    // have registered!
    register_executor_with_mempool();
    register_coordinator_with_mempool();
  }

  ~MempoolTestInfra()
  {
    // TODO(jhumphri): Implement clean shutdown for all RPC servers.
    // We need to call [executor_.shut_down()] so that the [Exector] destructor
    // does not block.
    executor_.shut_down();
  }

  void set_hogs(const ContractAddress &account, int group, BlockHeight age,
                uint128_t num_hogs)
  {
    CHECK_GE(group, 0);
    CHECK_LT(group, kHogAgeNumGroups);

    HogAge average;
    memset(&average, 0, sizeof(average));
    HogDBStatus<HogAge> res = db_.read_address_average_hog_age(account);
    if (res.has_value()) {
      average = *res;
    }

    average.address = account;
    average.average_ages[group] = age;
    average.num_hogs[group] = to_xdr(num_hogs);
    EXPECT_TRUE(db_.write_address_average_hog_age({average}));
  }

  void set_account_auth_data(const ContractAddress &account,
                             const AccountAuthData &auth_data)
  {
    CHECK(db_.write_address_account_auth_data(account, auth_data).has_value());
  }

  void set_dbtip(const DbTip &db_tip)
  {
    CHECK(db_.write_dbtip(db_tip).has_value());
  }

  // This getter is used by [auth_test.cc].
  MempoolCoordinatorRpc &mempool_coord_rpc()
  {
    return mempool_coord_server_.impl_;
  }

private:
  static constexpr std::string kTestContractFileName = "fibonacci.wasm";
  static constexpr uint128_t kStorageTokenAmount = 1'000'000'000;
  static constexpr size_t kRamDbBytes = 1024 * 1024 * 1024; // 1 GiB.

  static std::string address(const std::string &host, uint16_t port)
  {
    return std::format("{}:{}", host, port);
  }

  template<typename T>
  static auto create_client(const std::string& host, uint16_t port)
  {
    x23::UniqueFd fd =
        x23::tcpconnect(*x23::HostPort{host.c_str(), port}.to_addr());
    return x23::rpc_stream_logged(default_coropoll(), fd.release())
        ->client(T{});
  }

  void init_db_backend()
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

    db_ = HogDBDAO(RAMDB(std::move(*ramdb)));
    ctx_ = std::make_unique<RPCContext>(db_, trie_);
    set_dbtip({.height = 0, .hash = {}, .id = 0});
    ctx_->initialize_cost_model(StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = to_xdr(int128_t{12000}),
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = to_xdr(int128_t{12000}),
        .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
    }));
  }

  void init_contracts()
  {
    std::shared_ptr<Contract> contract = load_contract(kTestContractFileName);

    // Write contract to hogdb.
    auto bytes = x23::serialize(*contract);
    auto contract_hash = sha256(*contract);
    CHECK(db_.write_wasm_code(contract_hash, std::as_bytes(std::span(bytes)))
              .has_value());

    for (const ContractAddress &address : kAccountAddresses) {
      // Write deployment.
      uint32_t contract_size = bytes.size();
      CHECK(
          db_.write_address_metadata(address,
                                     DeployedContractHash{.hash = contract_hash,
                                                          .contract_size =
                                                              contract_size})
              .has_value());
      grant_initial_tokens_for_account(address);
    }
  }

  void grant_initial_tokens_for_account(const ContractAddress &address)
  {
    static_assert(kStorageTokenAmount >= 0);
    static_assert(kStorageTokenAmount <=
                  uint128_t{kHogDivisibility} * uint128_t{kTotalHogSupply});

    CHECK(db_.write_address_balance(address, kStorageTokenAmount).has_value());
    // TODO(jhumphri): Multiply by something other than 10. Or just give the
    // account unlimited hogs with UINT128_MAX.
    set_hogs(address, /*group=*/kHogAgeGroupToSchedule,
             /*age=*/1, /*num_hogs=*/5'000'000'000 * 10);
  }

  void register_db_with_mempool()
  {
    auto client = create_client<SchedDb1>(kAddress, sched_db_server_.port());
    std::string exec_address = address(kAddress, db_executor_server_->port());
    std::string sched_address = address(kAddress, db_scheduler_server_->port());
    client.scheddb_intro_shard({.protocol = version::kHogdProtocol,
                                .block_height = 0,
                                .address_for_exec = exec_address.c_str(),
                                .address_for_sched = sched_address.c_str()})();
  }

  void register_event_with_mempool()
  {
    auto client = create_client<SchedEvent1>(MempoolTestInfra::kAddress,
                                             sched_event_server_.port());
    auto r = client.schedevent_intro_event(
        {.protocol = version::kHogdProtocol,
         .upid = {},
         .address_for_exec =
             address(kAddress, event_exec_server_.port()).c_str(),
         .address_for_sched =
             address(kAddress, event_sched_server_.port()).c_str()})();
    CHECK_EQ(r, RPCError::kRPCNone);
  }

  void register_executor_with_mempool()
  {
    const sockaddr_storage coordinator_sockaddr =
        host_port_to_sockaddr(kAddress, sched_exec_server_.port());
    executor_.start(/*port_number=*/0, coordinator_sockaddr);
  }

  void register_coordinator_with_mempool()
  {
    auto client = create_client<SchedulerCoord1>(MempoolTestInfra::kAddress,
                                                 mempool_coord_server_.port());
    std::string coord_address =
        address(kAddress, coordinator_sched_server_.port());
    CoordCard card{.protocol = version::kHogdProtocol,
                   .address_for_sched = coord_address.c_str(),
                   .current_block_id = 0};
    CHECK_EQ(client.schedulercoord_intro_coord(card)(),
             CoordinatorRegistrationOutcome::CoordinatorRegistrationOK);
  }

  // The database server that the executor accesses.
  std::unique_ptr<HogDBExecutorServer> db_executor_server_;
  // The database server that the scheduler accesses.
  std::unique_ptr<HogDBSchedServer> db_scheduler_server_;
  // Various backend components required by the database.
  TmpDir db_dir_;
  HogDBDAO db_;
  MerkleTriePrefix trie_;
  std::unique_ptr<RPCContext> ctx_;

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
  CoordinatorSchedServer coordinator_sched_server_;

  // The mempool servers.
  RpcServer<MempoolDbRpc> sched_db_server_;
  RpcServer<MempoolEventRpc> sched_event_server_;
  RpcServer<MempoolExecutorRpc> sched_exec_server_;
  // We keep the [mempool_coord_server_] declaration below the various
  // database-related declarations above so that it is destructed first. We want
  // the database RPC server and its backend to be running so that the mempool
  // can flush its cache.
  MempoolCoordinatorServer mempool_coord_server_;
};

using fib_client_t = ContractTraitClient<fibonacci::Fibonacci>;

inline SignedTransactionBatch
generate_txs(fib_client_t &fib_client, size_t num_txs)
{
  constexpr char kProgramName[] = "slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  SignedTransactionBatch txs;
  txs.reserve(num_txs);
  for (uint64_t i = 0; i < num_txs; i++) {
    SignedTransaction tx{
        .tx = Transaction{
            .invocation = fib_client.make_invocation<kProgramName>(
                // TODO(jhumphri): How to get this number automatically?
                /*gas_amount=*/5'000'000'000, kFibonacciN),
            .uniquifier = {.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = i},
            .contracts_to_deploy = {}}};
    txs.push_back(tx);
  }
  return txs;
}

inline AccountAuthData
get_default_account()
{
  static std::array<PublicKeyConfigOpt, MAX_SIGNERS> public_keys;
  if (public_keys.empty()) {
    // Initialize [public_keys].
    for (size_t i = 0; i < MAX_SIGNERS; i++) {
      public_keys[i] = nullptr;
    }
  }
  CHECK_EQ(public_keys.size(), static_cast<size_t>(MAX_SIGNERS));
  constexpr NonceConfig kNonceConfig{.accepted_keys = 0, .required_weight = 0};

  return AccountAuthData{.public_keys = public_keys,
                         .gas_permissions = {},
                         .nonce_config = kNonceConfig};
}

inline Ed25519SecretKey
create_key_pair(AccountAuthData &auth_data, size_t idx, Weight weight)
{
  const auto [public_key, private_key] = ed25519_keygen();
  x23::Ptr<PublicKeyConfig> &pk_config = auth_data.public_keys[idx];
  pk_config = x23::Ptr<PublicKeyConfig>(new PublicKeyConfig());
  pk_config->key = PublicKey(CryptographicScheme::Ed25519);
  pk_config->key.ed25519() = public_key;
  pk_config->max_weight = weight;
  return private_key;
}

inline void
set_nonce_key(AccountAuthData &auth_data, size_t idx)
{
  CHECK_LT(idx, sizeof(auth_data.nonce_config.accepted_keys));
  auth_data.nonce_config.accepted_keys |= (1 << idx);
}

inline void
add_gas_config(AccountAuthData &auth_data, GasAmount gas,
               const x23::Vec<size_t> &keys, Weight required_weight)
{
  CHECK_LT(auth_data.gas_permissions.size(),
           static_cast<size_t>(MAX_GAS_CONFIGS));
  GasPermissionConfig config{.limit = gas,
                             .accepted_keys = 0,
                             .required_weight = required_weight};
  for (const size_t idx : keys) {
    config.accepted_keys |= (1 << idx);
  }
  auth_data.gas_permissions.push_back(config);
}

} // namespace hogd
