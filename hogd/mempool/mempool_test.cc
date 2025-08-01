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
#include "hogd/db_interface/db_coord_server.h"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/mempool/coordinator.h"
#include "hogd/mempool/mempool.h"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"
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

  static constexpr ContractAddress kContractAddress =
      NativeContractTestFramework::make_contract_address(3);

  MempoolTestServer()
    : db_pool_(x23::ThreadPool(kNumDbPoolThreads)),
      block_manager_(0, &in_memory_db_), coordinator_server_(kCoordinatorPort),
      executor_coord_server_(exec_card_, /*poll_threads=*/1),
      db_executor_server_(&block_manager_, kExecutorDbPort),
      db_coordinator_server_(&block_manager_), db_proxy_(kDbIdentifier),
      snapshot_cache_(
          std::make_unique<StorageSnapshotCache>(/*block_number=*/1, db_proxy_))
  {
    std::shared_ptr<Contract> contract =
        test::load_test_contract(kTestContractFileName);

    in_memory_db_.write_contract(sha256(*contract), contract);
    in_memory_db_.write_deployment(kContractAddress, sha256(*contract));

    set_up_grant_storage_token();

    db_coordinator_server_.start_server(kCoordinatorDbPort);

    executor_coord_server_.init_from_coord(
        x23::HostPort(kAddress.c_str(), kCoordinatorPort).show(),
        &kDbIdentifier);
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
  static constexpr StorageCostModel kCostModel{
      /*base_item_cost=*/12000, /*bytestring_cost_per_byte=*/1,
      /*rcq_cost_per_entry=*/5, /*rcq_cost_per_block=*/100,
      /*replay_cache_cost_per_entry=*/5};
  static constexpr int128_t kStorageTokenAmount = 1'000'000'000;

  void set_up_grant_storage_token()
  {
    static_assert(kStorageTokenAmount >= 0);
    static_assert(kStorageTokenAmount <=
                  int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

    AddressAndKey addrkey = {
        .address = kContractAddress,
        .key =
            StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
    in_memory_db_.write_item(
        addrkey, std::make_shared<const StorageItemWrapper>(kStorageTokenAmount,
                                                            &kCostModel));
  }

  x23::ThreadPool db_pool_;
  InMemoryDatabase in_memory_db_;
  BlockManager block_manager_;

  static constexpr LocalMockDB kDbIdentifier{.read_port_number_ =
                                                 kExecutorDbPort};
  ExecCard exec_card_ = {
      .protocol = "blah",
      .upid = {},
      .address = x23::HostPort(kAddress.c_str(), kCoordinatorPort).show()};
  CoordinatorServer coordinator_server_;
  ExecCoordServer executor_coord_server_;

  RPCDbExecutorServer db_executor_server_;
  DbCoordServer db_coordinator_server_;

  // TODO(jhumphri): Need to support these?
  DatabaseProxy db_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;
};

using fib_client_t = ContractTraitClient<Fibonacci>;

SignedTransactionBatch
generate_txs(fib_client_t &fib_client, size_t num_txs)
{
  constexpr char kProgramName[] = "slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  SignedTransactionBatch txs;
  txs.reserve(num_txs);
  for (int64_t i = 0; i < num_txs; i++) {
    SignedTransaction tx{
        .tx =
            Transaction{.id = {},
                        .created = i,
                        .invocation = fib_client.make_invocation<kProgramName>(
                            UINT64_MAX, kFibonacciN),
                        .contracts_to_deploy = {}}};
    txs.push_back(tx);
  }
  return txs;
}

struct Clients {
  std::unique_ptr<x23::ThreadPool> db_pool;
  std::unique_ptr<x23::CoroPoll> db_cp;

  std::unique_ptr<x23::ThreadPool> exec_pool;
  std::unique_ptr<x23::CoroPoll> exec_cp;

  std::unique_ptr<Mempool> interface;
};

Clients
create_clients()
{
  auto db_pool = std::make_unique<x23::ThreadPool>(1);
  auto db_cp = std::make_unique<x23::CoroPoll>(*db_pool);
  Mempool::coord_client_t coord_client = [&] -> Mempool::coord_client_t {
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
  Mempool::exec_client_t exec_client = [&] -> Mempool::exec_client_t {
    auto fd =
        exec_cp->tcpconnect(MempoolTestServer::kAddress,
                            std::to_string(MempoolTestServer::kExecutorPort))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to executor server");
    }
    auto client_config = x23::rpc_stream_logged(*exec_cp, *fd);
    return client_config.template client<ExecutorCoord1>();
  }();

  auto interface = std::make_unique<Mempool>(std::move(coord_client),
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
  fib_client_t fib_client(MempoolTestServer::kContractAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients();

  clients.interface->schedulercoord_submit_unvetted({txs[0]})();
}

TEST(Mempool, SubmitVetted)
{
  fib_client_t fib_client(MempoolTestServer::kContractAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients();

  ValidatorID validator_id{};
  clients.interface->schedulercoord_submit_vetted(validator_id, {txs[0]})();
}

TEST(Mempool, Nominate)
{
  fib_client_t fib_client(MempoolTestServer::kContractAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients();

  ValidatorID validator_id{};
  clients.interface->schedulercoord_submit_vetted(validator_id, {txs[0]})();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = clients.interface->schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
}

TEST(Mempool, NominateBatch)
{
  // x23::RpcLogger::default_logfd = 1;

  fib_client_t fib_client(MempoolTestServer::kContractAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);
  for (const SignedTransaction &tx : txs) {
    printf("check size %zu\n", tx.tx.contracts_to_deploy.size());
  }

  MempoolTestServer server;
  Clients clients = create_clients();

  ValidatorID validator_id{};
  clients.interface->schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res = clients.interface->schedulercoord_nominate(db_tip)();
  EXPECT_EQ(res.outcome(), SchedulerNominationOutcome::SchedulerNominationOK);
}

TEST(Mempool, Validate)
{
  fib_client_t fib_client(MempoolTestServer::kContractAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients();

  ValidatorID validator_id{};
  clients.interface->schedulercoord_submit_vetted(validator_id, {txs[0]})();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate = clients.interface->schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  clients.interface->schedulercoord_submit_vetted(validator_id, {txs[0]})();
  auto res_validate = clients.interface->schedulercoord_validate(
      db_tip, res_nominate.scheduler_nomination().nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateBatch)
{
  fib_client_t fib_client(MempoolTestServer::kContractAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);
  printf("here, txs size is %zu\n", txs.size());

  MempoolTestServer server;
  Clients clients = create_clients();

  ValidatorID validator_id{};
  clients.interface->schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate = clients.interface->schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  clients.interface->schedulercoord_submit_vetted(validator_id, txs)();
  auto res_validate = clients.interface->schedulercoord_validate(
      db_tip, res_nominate.scheduler_nomination().nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
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
