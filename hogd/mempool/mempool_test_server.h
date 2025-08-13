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
  static constexpr ContractAddress kContractAddress =
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
      ret = write_address_wasm_hash(*db_, kContractAddress, contract_hash);
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
  static constexpr int128_t kStorageTokenAmount = 1'000'000'000;
  static constexpr size_t kRamDbBytes = 1024 * 1024 * 400; // 400 MiB.

  void set_up_grant_storage_token()
  {
    static_assert(kStorageTokenAmount >= 0);
    static_assert(kStorageTokenAmount <=
                  int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

    // Convert ContractAddress to Address - both are std::array<uint8_t, 32>
    Address addr(kContractAddress);
    auto ret = write_address_balance(*db_, addr, kStorageTokenAmount);
    if (!ret) {
      throw std::runtime_error("Failed to grant storage token");
    }
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

} // namespace hogd
