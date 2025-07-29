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

  ~MempoolTestServer()
  {
    // We need to call [executor_coord_server_.shut_down()] so that the
    // [ExecCoordServer] destructor does not block.
    executor_coord_server_.shut_down();
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

  LocalMockDB db_proxy_{.read_port_number_ = kCoordinatorDbPort};
  ExecCard exec_card_ = {
      .protocol = "blah",
      .upid = {},
      .address = (kAddress + ":" + std::to_string(kCoordinatorPort)).c_str()};
  CoordinatorServer coordinator_server_;
  ExecCoordServer executor_coord_server_;

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

struct Clients {
  std::unique_ptr<x23::ThreadPool> db_pool;
  std::unique_ptr<x23::CoroPoll> db_cp;

  std::unique_ptr<x23::ThreadPool> exec_pool;
  std::unique_ptr<x23::CoroPoll> exec_cp;

  std::unique_ptr<MempoolRpcInterface> interface;
};

Clients
create_clients()
{
  auto db_pool = std::make_unique<x23::ThreadPool>(1);
  auto db_cp = std::make_unique<x23::CoroPoll>(*db_pool);
  MempoolRpcInterface::coord_client_t coord_client =
      [&] -> MempoolRpcInterface::coord_client_t {
    auto fd = db_cp->tcpconnect(MempoolTestServer::kAddress,
                                std::to_string(
                                    MempoolTestServer::kCoordinatorDbPort))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to coord server");
    }
    auto client_config = x23::rpc_stream_logged(*db_cp, *fd);
    return client_config.template client<DbCoord1>();
  }();

  auto exec_pool = std::make_unique<x23::ThreadPool>(1);
  auto exec_cp = std::make_unique<x23::CoroPoll>(*exec_pool);
  MempoolRpcInterface::exec_client_t exec_client =
      [&] -> MempoolRpcInterface::exec_client_t {
    auto fd =
        exec_cp->tcpconnect(MempoolTestServer::kAddress,
                            std::to_string(MempoolTestServer::kExecutorPort))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to executor server");
    }
    auto client_config = x23::rpc_stream_logged(*exec_cp, *fd);
    return client_config.template client<ExecutorCoord1>();
  }();

  auto interface =
      std::make_unique<MempoolRpcInterface>(std::move(coord_client),
                                            std::move(exec_client));
  return {.db_pool = std::move(db_pool),
          .db_cp = std::move(db_cp),
          .exec_pool = std::move(exec_pool),
          .exec_cp = std::move(exec_cp),
          .interface = std::move(interface)};
}

constexpr size_t kNumTxs = 100;

} // namespace

TEST(Mempool, SubmitUnvetted)
{
  fib_client_t fib_client(ContractAddress{});
  std::vector<SignedTransaction> txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients();

  clients.interface->schedulercoord_submit_unvetted({txs[0]})();
}

TEST(Mempool, SubmitVetted)
{
  fib_client_t fib_client(ContractAddress{});
  std::vector<SignedTransaction> txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients();

  ValidatorID validator_id{};
  clients.interface->schedulercoord_submit_vetted(validator_id, {txs[0]})();
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
