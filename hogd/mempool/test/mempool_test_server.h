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

class MempoolTestServer {
public:
  static constexpr std::string kAddress = "127.0.0.1";
  static constexpr std::array<ContractAddress, 4> kAccountAddresses = {
      NativeContractTestFramework::make_contract_address(3),
      NativeContractTestFramework::make_contract_address(4),
      NativeContractTestFramework::make_contract_address(5),
      NativeContractTestFramework::make_contract_address(6)};

  MempoolTestServer()
    : sched_db_port_(sched_db_server_.start_server(/*port=*/0)),
      sched_event_port_(sched_event_server_.start_server(/*port=*/0)),
      sched_exec_port_(sched_exec_server_.start_server(/*port=*/0)),
      coordinator_sched_port_(
          coordinator_sched_server_.start_server(/*port=*/0)),
      executor_(kAddress, sched_exec_port_)
  {
    // Multiple tests run within the same process, so we need to clear the
    // registration at the beginning of each test.
    Registration::registration().clear();

    {
      // check the database's integrity and find the highest valid LSN.
      DBStatus<uint64_t> lsn =
          Database::check(db_dir_.directory(), kRamDbBytes);
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

      db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
      db_executor_server_->start_server(/*port=*/0);
      db_executor_port_ = db_executor_server_->port();

      db_scheduler_server_ = std::make_unique<HogDBSchedServer>(*ctx_);
      db_scheduler_server_->start_server(/*port=*/0);
      db_scheduler_port_ = db_scheduler_server_->port();

      event_sched_port_ =
          event_server_sched_rpc_.start_server(/*port_number=*/0);
      event_exec_port_ = event_server_exec_rpc_.start_server(/*port_number=*/0);
    }

    {
      std::shared_ptr<Contract> contract = load_contract(kTestContractFileName);

      // Write contract to hogdb.
      auto bytes = x23::serialize(*contract);
      auto contract_hash = sha256(*contract);
      auto ret =
          db_.write_wasm_code(contract_hash, std::as_bytes(std::span(bytes)));
      if (!ret) {
        throw std::runtime_error("Failed to write contract");
      }

      for (const ContractAddress &address : kAccountAddresses) {
        // Write deployment.
        uint32_t contract_size = bytes.size();
        ret = db_.write_address_metadata(
            address, DeployedContractHash{.hash = contract_hash,
                                          .contract_size = contract_size});
        if (!ret) {
          throw std::runtime_error("Failed to write deployment");
        }

        set_up_grant_storage_token(address);
      }
    }

    {
      // Now have the db register itself with the mempool.
      x23::ThreadPool pool;
      x23::CoroPoll cp(pool);
      x23::UniqueFd fd = x23::tcpconnect(
          *x23::HostPort{MempoolTestServer::kAddress.c_str(), sched_db_port_}
               .to_addr());
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef> client =
          x23::rpc_stream_logged(cp, fd.release())->client(SchedDb1{});

      std::string exec_address =
          std::format("{}:{}", kAddress, db_executor_port_);
      std::string sched_address =
          std::format("{}:{}", kAddress, db_scheduler_server_->port());
      client.scheddb_intro_shard(
          {.protocol = version::kHogdProtocol,
           .block_height = 0,
           .address_for_coord = "fail",
           .address_for_exec = exec_address.c_str(),
           .address_for_sched = sched_address.c_str()})();

      // make the event server register itself with the mempool.
      fd = x23::tcpconnect(*x23::HostPort{MempoolTestServer::kAddress.c_str(),
                                          sched_event_server_.port()}
                                .to_addr());
      SchedEvent1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>
          event_client =
              x23::rpc_stream_logged(cp, fd.release())->client(SchedEvent1{});

      auto r = event_client.schedevent_intro_event(
          {.protocol = version::kHogdProtocol,
           .upid = {},
           .address_for_exec =
               std::format("{}:{}", kAddress, event_exec_server_.port())
                   .c_str(),
           .address_for_sched =
               std::format("{}:{}", kAddress, event_sched_server_.port())
                   .c_str()})();
      CHECK_EQ(r, RPCError::kRPCNone);
    }

    {
      // Now have the executor register itself with the mempool.
      executor_.start(/*port_number=*/0, kAddress);
    }

    uint16_t sched_coord_port = mempool_coord_server_.start_server(/*port=*/0);
    {
      x23::ThreadPool pool;
      x23::CoroPoll cp(pool);
      x23::UniqueFd fd = x23::tcpconnect(
          *x23::HostPort{MempoolTestServer::kAddress.c_str(), sched_coord_port}
               .to_addr());
      SchedulerCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>
          client = x23::rpc_stream_logged(cp, fd.release())
                       ->client(SchedulerCoord1{});

      std::string coord_address =
          std::format("{}:{}", kAddress, coordinator_sched_server_.port());
      CoordCard card{.protocol = version::kHogdProtocol,
                     .address_for_sched = coord_address.c_str(),
                     .current_block_id = 0};
      CHECK_EQ(client.schedulercoord_intro_coord(card)(),
               CoordinatorRegistrationOutcome::CoordinatorRegistrationOK);
    }
    wait_for_registration();
  }

  ~MempoolTestServer()
  {
    // We need to call [executor_.shut_down()] so that the [Exector] destructor
    // does not block.
    executor_.shut_down();
  }

  static void wait_for_registration()
  {
    Registration &registration = Registration::registration();
    while (!registration.shards_registered(/*cards=*/nullptr)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    while (!registration.event_registered(/*card=*/nullptr)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    while (!registration.executors_registered(/*cards=*/nullptr)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // We manually call [schedulercoord_intro_coord()], so
    // [registration.coordinator_registered()] must be true.
    CHECK(registration.coordinator_registered(/*card=*/nullptr));
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

  void set_costmodel(const StorageCostModel &cost_model)
  {
    CHECK(db_.write_cost_model(cost_model.to_xdr()).has_value());
  }

  uint16_t coordinator_sched_port() const
  {
    return coordinator_sched_server_.port();
  }
  uint16_t db_executor_port() const { return db_executor_server_->port(); }
  uint16_t db_scheduler_port() const { return db_scheduler_server_->port(); }
  uint16_t event_sched_port() const { return event_sched_server_->port(); }

  MempoolDbRpc &sched_db_rpc() { return sched_db_server_.get_impl(); }
  MempoolExecutorRpc &sched_exec_rpc() { return sched_exec_server_.get_impl(); }
  MempoolCoordinatorRpc &mempool_coord_rpc()
  {
    return mempool_coord_server_.get_impl();
  }

private:
  static constexpr std::string kTestContractFileName = "fibonacci.wasm";
  static constexpr uint128_t kStorageTokenAmount = 1'000'000'000;
  static constexpr size_t kRamDbBytes = 1024 * 1024 * 1024; // 1 GiB.

  void set_up_grant_storage_token(const ContractAddress &address)
  {
    static_assert(kStorageTokenAmount >= 0);
    static_assert(kStorageTokenAmount <=
                  uint128_t{kHogDivisibility} * uint128_t{kTotalHogSupply});

    // Convert ContractAddress to Address - both are std::array<uint8_t, 32>
    Address addr(address);
    auto ret = db_.write_address_balance(addr, kStorageTokenAmount);
    if (!ret) {
      throw std::runtime_error("Failed to grant storage token");
    }

    // TODO(jhumphri): Multiply by something other than 10. Or just give the
    // account unlimited hogs with UINT128_MAX.
    set_hogs(address, /*group=*/kHogAgeGroupToSchedule,
             /*age=*/1, /*num_hogs=*/5'000'000'000 * 10);
  }

  RpcServer<MempoolDbRpc> sched_db_server_;
  uint16_t sched_db_port_;

  RpcServer<MempoolEventRpc> sched_event_server_;
  uint16_t sched_event_port_;

  RpcServer<MempoolExecutorRpc> sched_exec_server_;
  uint16_t sched_exec_port_;

  // This is a fake coordinator server that the mempool notifies when txs
  // become vetted.
  CoordinatorSchedServer coordinator_sched_server_;
  uint16_t coordinator_sched_port_;

  // The executor server.
  Executor executor_;

  // The database server that the executor accesses.
  std::unique_ptr<HogDBExecutorServer> db_executor_server_;
  uint16_t db_executor_port_;

  // The database server that the scheduler accesses.
  std::unique_ptr<HogDBSchedServer> db_scheduler_server_;
  uint16_t db_scheduler_port_;

  // The database server that the mempool accesses.
  TmpDir db_dir_;
  HogDBDAO db_;
  MerkleTriePrefix trie_;
  std::unique_ptr<RPCContext> ctx_;

  // The event server that the mempool accesses.
  TestEventAggregator event_server_;
  TestEventAggregatorSchedRpc event_server_sched_rpc_ =
      TestEventAggregatorSchedRpc(event_server_);
  TestEventAggregatorExecRpc event_server_exec_rpc_ =
      TestEventAggregatorExecRpc(event_server_);
  uint16_t event_sched_port_;
  uint16_t event_exec_port_;

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
