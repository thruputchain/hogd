#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "contract.hh"
#include "coord.hh"
#include "coord_sched.hh"
#include "db_coord.hh"
#include "discovery.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_coord.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/mempool/coordinator.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/types.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"

namespace hogd {

class MempoolTestServer {
public:
  static constexpr std::string kAddress = "127.0.0.1";
  static constexpr ContractAddress kAccountAddress =
      NativeContractTestFramework::make_contract_address(3);

  MempoolTestServer()
    : coordinator_port_(coordinator_server_.start_server(/*port_number=*/0)),
      exec_card_{.protocol = "blah",
                 .upid = {},
                 .address =
                     x23::HostPort(kAddress.c_str(), coordinator_port_).show()},
      executor_coord_server_(exec_card_, /*poll_threads=*/1)
  {
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

      db_ = std::make_unique<HogDatabase>(std::move(*ramdb));
      ctx_ = std::make_unique<RPCContext>(*db_, trie_);

      db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
      CHECK(db_executor_server_->start_server(/*port=*/0).has_value());
      executor_db_port_ = db_executor_server_->port();

      db_coordinator_server_ = std::make_unique<HogDBCoordServer>(*ctx_);
      db_coordinator_server_->start_server(/*port=*/0);
      coordinator_db_port_ = db_coordinator_server_->port();
    }

    {
      std::shared_ptr<Contract> contract =
          test::load_test_contract(kTestContractFileName);

      // Write contract to hogdb
      auto bytes = x23::serialize(*contract);
      auto contract_hash = sha256(*contract);
      auto ret =
          write_wasm_code(*db_, contract_hash, std::as_bytes(std::span(bytes)));
      if (!ret) {
        throw std::runtime_error("Failed to write contract");
      }

      // Write deployment
      ret = write_address_wasm_hash(*db_, kAccountAddress, contract_hash);
      if (!ret) {
        throw std::runtime_error("Failed to write deployment");
      }

      set_up_grant_storage_token();
    }

    coordinator_sched_port_ =
        coordinator_sched_server_.start_server(/*port_number=*/0);

    {
      LocalMockDB db_identifier{.read_port_number_ = executor_db_port_};
      executor_coord_server_.init_from_coord(
          x23::HostPort(kAddress.c_str(), coordinator_port_).show(),
          &db_identifier);
      executor_port_ = executor_coord_server_.start_server(/*port_number=*/0);
    }
  }

  ~MempoolTestServer()
  {
    // We need to call [executor_coord_server_.shut_down()] so that the
    // [ExecCoordServer] destructor does not block.
    executor_coord_server_.shut_down();
  }

  uint16_t coordinator_sched_port() const { return coordinator_sched_port_; }
  uint16_t executor_db_port() const { return executor_db_port_; }
  uint16_t coordinator_port() const { return coordinator_port_; }
  uint16_t coordinator_db_port() const { return coordinator_db_port_; }
  uint16_t executor_port() const { return executor_port_; }

private:
  static constexpr std::string kTestContractFileName = "fibonacci.wasm";
  static constexpr uint128_t kStorageTokenAmount = 1'000'000'000;
  static constexpr size_t kRamDbBytes = 1024 * 1024 * 400; // 400 MiB.

  void set_up_grant_storage_token()
  {
    static_assert(kStorageTokenAmount >= 0);
    static_assert(kStorageTokenAmount <=
                  uint128_t{kHogDivisibility} * uint128_t{kTotalHogSupply});

    // Convert ContractAddress to Address - both are std::array<uint8_t, 32>
    Address addr(kAccountAddress);
    auto ret = write_address_balance(*db_, addr, kStorageTokenAmount);
    if (!ret) {
      throw std::runtime_error("Failed to grant storage token");
    }

    HogAge average;
    memset(&average, 0, sizeof(average));
    average.address = kAccountAddress;
    average.num_hogs[kHogAgeGroupToSchedule] =
        // TODO(jhumphri): Multiply by something other than 10. Or just give the
        // account unlimited hogs with UINT128_MAX.
        to_xdr(static_cast<uint128_t>(5'000'000'000 * 10));
    average.average_ages[kHogAgeGroupToSchedule] = 1;
    EXPECT_TRUE(write_address_average_hog_age(*db_, {average}));
  }

  // This is a fake coordinator server that the executor registers itself with
  // via RPC.
  Coordinator coordinator_server_;
  uint16_t coordinator_port_;

  // This is a fake coordinator server that the mempool notifies when txs become
  // vetted.
  CoordinatorSched coordinator_sched_server_;
  uint16_t coordinator_sched_port_;

  // The executor server.
  ExecCard exec_card_;
  ExecCoordServer executor_coord_server_;
  uint16_t executor_port_;

  // The database server that the executor accesses.
  std::unique_ptr<HogDBExecutorServer> db_executor_server_;
  uint16_t executor_db_port_;

  // The database server that the mempool accesses.
  TmpDir db_dir_;
  std::unique_ptr<HogDatabase> db_;
  MerkleTriePrefix trie_;
  std::unique_ptr<RPCContext> ctx_;
  std::unique_ptr<HogDBCoordServer> db_coordinator_server_;
  uint16_t coordinator_db_port_;
};

using fib_client_t = ContractTraitClient<Fibonacci>;

inline SignedTransactionBatch
generate_txs(fib_client_t &fib_client, size_t num_txs)
{
  constexpr char kProgramName[] = "slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  SignedTransactionBatch txs;
  txs.reserve(num_txs);
  for (uint64_t i = 0; i < num_txs; i++) {
    Timestamp created = i;
    SignedTransaction tx{
        .tx = Transaction{
            .origin_id = {},
            .created = created,
            .invocation = fib_client.make_invocation<kProgramName>(
                // TODO(jhumphri): How to get this number automatically?
                /*gas_amount=*/5'000'000'000, kFibonacciN),
            .contracts_to_deploy = {}}};
    txs.push_back(tx);
  }
  return txs;
}

struct Clients {
  std::unique_ptr<x23::ThreadPool> coord_pool;
  std::unique_ptr<x23::CoroPoll> coord_cp;

  std::unique_ptr<x23::ThreadPool> db_pool;
  std::unique_ptr<x23::CoroPoll> db_cp;

  std::unique_ptr<x23::ThreadPool> exec_pool;
  std::unique_ptr<x23::CoroPoll> exec_cp;

  std::unique_ptr<Mempool> interface;
};

inline Clients
create_clients(const MempoolTestServer &server)
{
  auto coord_pool = std::make_unique<x23::ThreadPool>(1);
  auto coord_cp = std::make_unique<x23::CoroPoll>(*coord_pool);
  coord_client_t coord_client = [&] -> coord_client_t {
    auto fd =
        coord_cp->tcpconnect(MempoolTestServer::kAddress,
                             std::to_string(server.coordinator_sched_port()))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to coord server");
    }
    auto client_config = x23::rpc_stream_logged(*coord_cp, *fd);
    return client_config.template client<CoordScheduler1>();
  }();

  auto db_pool = std::make_unique<x23::ThreadPool>(1);
  auto db_cp = std::make_unique<x23::CoroPoll>(*db_pool);
  db_client_t db_client = [&] -> db_client_t {
    auto fd = db_cp->tcpconnect(MempoolTestServer::kAddress,
                                std::to_string(server.coordinator_db_port()))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to db server");
    }
    auto client_config = x23::rpc_stream_logged(*db_cp, *fd);
    return client_config.template client<DbCoord1>();
  }();

  auto exec_pool = std::make_unique<x23::ThreadPool>(1);
  auto exec_cp = std::make_unique<x23::CoroPoll>(*exec_pool);
  exec_client_t exec_client = [&] -> exec_client_t {
    auto fd = exec_cp->tcpconnect(MempoolTestServer::kAddress,
                                  std::to_string(server.executor_port()))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to executor server");
    }
    auto client_config = x23::rpc_stream_logged(*exec_cp, *fd);
    return client_config.template client<ExecutorCoord1>();
  }();

  auto interface =
      std::make_unique<Mempool>(std::move(coord_client), std::move(db_client),
                                std::move(exec_client));
  return {.coord_pool = std::move(coord_pool),
          .coord_cp = std::move(coord_cp),
          .db_pool = std::move(db_pool),
          .db_cp = std::move(db_cp),
          .exec_pool = std::move(exec_pool),
          .exec_cp = std::move(exec_cp),
          .interface = std::move(interface)};
}

constexpr size_t kNumTxs = 100;

} // namespace hogd
