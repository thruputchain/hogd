#include <cstddef>
#include <cstdio>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <iostream>

#include <map>
#include <memory>
#include <print>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/mempool/coordinator.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/rpc_interface.h"
#include "types.hh"

namespace hogd {

namespace {

class MempoolTestServer {
public:
  static constexpr std::string kAddress = "127.0.0.1";
  static constexpr uint16_t kExecutorDbPort = 10000;
  static constexpr uint16_t kCoordinatorDbPort = 10001;
  static constexpr uint16_t kCoordinatorPort = 10002;
  static constexpr uint16_t kExecutorPort = 10003;

  MempoolTestServer()
    : db_pool_(x23::ThreadPool(kNumDbPoolThreads)),
      cost_model_(12000, 1, 5, 100, 5), block_manager_(0, &in_memory_db_),
      coordinator_server_(kCoordinatorPort),
      executor_coord_server_(exec_card_, /*poll_threads=*/1),
      db_executor_server_(&block_manager_, kExecutorDbPort),
      db_coordinator_server_(&block_manager_)
  {
    ContractAddress contract_addr;
    std::shared_ptr<Contract> contract =
        test::load_test_contract(kTestContractFileName);

    in_memory_db_.write_contract(sha256(*contract), contract);
    in_memory_db_.write_deployment(contract_addr, sha256(*contract));

    setup_grant_storage_token(in_memory_db_, cost_model_, contract_addr,
                              1'000'000'000);

    db_coordinator_server_.start_server(kCoordinatorDbPort);

    executor_coord_server_.init_from_coord(kAddress + ":" +
                                               std::to_string(kCoordinatorPort),
                                           &db_proxy_);
    executor_coord_server_.start_server(kExecutorPort);
  }

private:
  static constexpr std::string kTestContractFileName = "fibonacci.wasm";
  static constexpr size_t kNumDbPoolThreads = 16;

  static void setup_grant_storage_token(InMemoryDatabase &in_memory_db,
                                        StorageCostModel const &cost_model,
                                        ContractAddress const &address,
                                        int128_t amount)
  {
    CHECK_GE(amount, 0);
    CHECK_LE(amount, int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

    AddressAndKey addrkey = {
        .address = address,
        .key =
            StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

    in_memory_db.write_item(addrkey, std::make_shared<const StorageItemWrapper>(
                                         amount, &cost_model));
  }

  x23::ThreadPool db_pool_;
  InMemoryDatabase in_memory_db_;
  StorageCostModel cost_model_;
  BlockManager block_manager_;
  ExecCard exec_card_ = {
      .protocol = "blah",
      .upid = {},
      .address = (kAddress + ":" + std::to_string(kCoordinatorPort)).c_str()};
  LocalMockDB db_proxy_{.read_port_number_ = kCoordinatorDbPort};
  CoordinatorServer coordinator_server_;
  ExecCoordServer executor_coord_server_;
  // TODO(jhumphri): Add [ExecCoordServer]. Keep the [db_executor_server_]
  // variable below because I think we need it.
  RPCDbExecutorServer db_executor_server_;
  DbCoordServer db_coordinator_server_;
};

using fib_client_t = ContractTraitClient<Fibonacci>;

std::vector<SignedTransaction>
generate_txs(fib_client_t &fib_client, size_t num_txs)
{
  constexpr char kProgramName[] = "slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  std::vector<SignedTransaction> txs;
  txs.reserve(num_txs);
  for (size_t i = 0; i < num_txs; i++) {
    SignedTransaction tx{
        .tx =
            Transaction{.invocation = fib_client.make_invocation<kProgramName>(
                            UINT64_MAX, kFibonacciN)}};
    txs.push_back(tx);
  }
  return txs;
}

} // namespace

TEST(Mempool, HelloWorld)
{
  constexpr size_t kNumTxs = 100;

  fib_client_t fib_client(ContractAddress{});
  std::vector<SignedTransaction> txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;

  x23::ThreadPool db_pool = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(db_pool);
  MempoolRpcInterface::coord_client_t coord_client =
      [&] -> MempoolRpcInterface::coord_client_t {
    auto fd =
        cp_.tcpconnect(MempoolTestServer::kAddress,
                       std::to_string(MempoolTestServer::kCoordinatorDbPort))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to coord server");
    }
    auto client_config = x23::rpc_stream_logged(cp_, *fd);
    return client_config.template client<DbCoord1>();
  }();

  x23::ThreadPool exec_pool = x23::ThreadPool(1);
  x23::CoroPoll exec_cp = x23::CoroPoll(exec_pool);
  MempoolRpcInterface::exec_client_t exec_client =
      [&] -> MempoolRpcInterface::exec_client_t {
    auto fd =
        cp_.tcpconnect(MempoolTestServer::kAddress,
                       std::to_string(MempoolTestServer::kExecutorPort))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to executor server");
    }
    auto client_config = x23::rpc_stream_logged(cp_, *fd);
    return client_config.template client<ExecutorCoord1>();
  }();

  MempoolRpcInterface interface(std::move(coord_client),
                                std::move(exec_client));

  {
    SignedTransaction tx{};
    interface.schedulercoord_submit_unvetted({tx})();
  }

  {
    ValidatorID validator_id{};
    SignedTransaction tx{};
    interface.schedulercoord_submit_vetted(validator_id, {tx})();
  }

  // TODO(jhumphri): Exit gracefully.
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
