#pragma once

#include <array>
#include <cstring>
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
#include "coord.hh"
#include "coord_sched.hh"
#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_coord.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/mempool/coordinator.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/types.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"

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

      db_ = HogDBDAO(RAMDB(std::move(*ramdb)));
      ctx_ = std::make_unique<RPCContext>(db_, trie_);

      db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
      db_executor_server_->start_server(/*port=*/0);
      executor_db_port_ = db_executor_server_->port();

      db_coordinator_server_ = std::make_unique<HogDBCoordServer>(*ctx_);
      db_coordinator_server_->start_server(/*port=*/0);
      db_coordinator_port_ = db_coordinator_server_->port();

      db_scheduler_server_ = std::make_unique<HogDBSchedServer>(*ctx_);
      db_scheduler_server_->start_server(/*port=*/0);
      db_scheduler_port_ = db_scheduler_server_->port();
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
        ret = db_.write_address_wasm_hash(address, contract_hash);
        if (!ret) {
          throw std::runtime_error("Failed to write deployment");
        }

        set_up_grant_storage_token(address);
      }
    }

    coordinator_sched_port_ =
        coordinator_sched_server_.start_server(/*port_number=*/0);

    {
      LocalMockDB db_identifier{.read_port_number_ = executor_db_port_};
      executor_coord_server_.init_from_coord(&db_identifier);
      executor_port_ = executor_coord_server_.start_server(/*port_number=*/0);
    }
  }

  ~MempoolTestServer()
  {
    // We need to call [executor_coord_server_.shut_down()] so that the
    // [ExecCoordServer] destructor does not block.
    executor_coord_server_.shut_down();
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

  uint16_t coordinator_sched_port() const { return coordinator_sched_port_; }
  uint16_t executor_db_port() const { return executor_db_port_; }
  uint16_t coordinator_port() const { return coordinator_port_; }
  uint16_t db_coordinator_port() const { return db_coordinator_port_; }
  uint16_t db_scheduler_port() const { return db_scheduler_port_; }
  uint16_t executor_port() const { return executor_port_; }

private:
  static constexpr std::string kTestContractFileName = "fibonacci.wasm";
  static constexpr uint128_t kStorageTokenAmount = 1'000'000'000;
  static constexpr size_t kRamDbBytes = 1024 * 1024 * 400; // 400 MiB.

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

  // This is a fake coordinator server that the executor registers itself with
  // via RPC.
  Coordinator coordinator_server_;
  uint16_t coordinator_port_;

  // This is a fake coordinator server that the mempool notifies when txs
  // become vetted.
  CoordinatorSched coordinator_sched_server_;
  uint16_t coordinator_sched_port_;

  // The executor server.
  ExecCard exec_card_;
  ExecCoordServer executor_coord_server_;
  uint16_t executor_port_;

  // The database server that the executor accesses.
  std::unique_ptr<HogDBExecutorServer> db_executor_server_;
  uint16_t executor_db_port_;

  // The database server that the coordinator accesses.
  std::unique_ptr<HogDBCoordServer> db_coordinator_server_;
  uint16_t db_coordinator_port_;

  // The database server that the scheduler accesses.
  std::unique_ptr<HogDBSchedServer> db_scheduler_server_;
  uint16_t db_scheduler_port_;

  // The database server that the mempool accesses.
  TmpDir db_dir_;
  HogDBDAO db_;
  MerkleTriePrefix trie_;
  std::unique_ptr<RPCContext> ctx_;
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

struct Clients {
  std::unique_ptr<x23::ThreadPool> coord_pool;
  std::unique_ptr<x23::CoroPoll> coord_cp;

  std::unique_ptr<x23::ThreadPool> db_coord_pool;
  std::unique_ptr<x23::CoroPoll> db_coord_cp;

  std::unique_ptr<x23::ThreadPool> db_sched_pool;
  std::unique_ptr<x23::CoroPoll> db_sched_cp;

  std::unique_ptr<x23::ThreadPool> exec_pool;
  std::unique_ptr<x23::CoroPoll> exec_cp;

  std::unique_ptr<Mempool> interface;
};

inline Clients
create_clients(const MempoolTestServer &server)
{
  // Mempool-Coordinator RPC client.
  auto coord_pool = std::make_unique<x23::ThreadPool>(1);
  auto coord_cp = std::make_unique<x23::CoroPoll>(*coord_pool);
  x23::UniqueFd coord_fd =
      x23::tcpconnect(*x23::HostPort{MempoolTestServer::kAddress.c_str(),
                                     server.coordinator_sched_port()}
                           .to_addr());
  coord_client_t coord_client =
      x23::rpc_stream_logged(*coord_cp, coord_fd.release())
          ->client(CoordScheduler1{});

  // Coordinator-DB RPC client.
  auto db_coord_pool = std::make_unique<x23::ThreadPool>(1);
  auto db_coord_cp = std::make_unique<x23::CoroPoll>(*db_coord_pool);
  x23::UniqueFd db_coord_fd =
      x23::tcpconnect(*x23::HostPort{MempoolTestServer::kAddress.c_str(),
                                     server.db_coordinator_port()}
                           .to_addr());
  db_coord_client_t db_coord_client =
      x23::rpc_stream_logged(*db_coord_cp, db_coord_fd.release())
          ->client(DbCoord1{});

  // Mempool-DB RPC client.
  auto db_sched_pool = std::make_unique<x23::ThreadPool>(1);
  auto db_sched_cp = std::make_unique<x23::CoroPoll>(*db_sched_pool);
  x23::UniqueFd db_sched_fd =
      x23::tcpconnect(*x23::HostPort{MempoolTestServer::kAddress.c_str(),
                                     server.db_scheduler_port()}
                           .to_addr());
  db_sched_client_t db_sched_client =
      x23::rpc_stream_logged(*db_sched_cp, db_sched_fd.release())
          ->client(DbSched1{});

  // Mempool-Executor RPC client.
  auto exec_pool = std::make_unique<x23::ThreadPool>(1);
  auto exec_cp = std::make_unique<x23::CoroPoll>(*exec_pool);
  x23::UniqueFd exec_fd =
      x23::tcpconnect(*x23::HostPort{MempoolTestServer::kAddress.c_str(),
                                     server.executor_port()}
                           .to_addr());
  exec_client_t exec_client =
      x23::rpc_stream_logged(*exec_cp, exec_fd.release())
          ->client(ExecutorCoord1{});

  auto interface = std::make_unique<Mempool>(/*most_recent_block-*/ 0,
                                             std::move(coord_client),
                                             std::move(db_coord_client),
                                             std::move(db_sched_client),
                                             std::move(exec_client));
  return {.coord_pool = std::move(coord_pool),
          .coord_cp = std::move(coord_cp),
          .db_coord_pool = std::move(db_coord_pool),
          .db_coord_cp = std::move(db_coord_cp),
          .db_sched_pool = std::move(db_sched_pool),
          .db_sched_cp = std::move(db_sched_cp),
          .exec_pool = std::move(exec_pool),
          .exec_cp = std::move(exec_cp),
          .interface = std::move(interface)};
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

} // namespace hogd
