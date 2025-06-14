#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/init.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {

struct BenchmarkState {

  BenchmarkState(WorkerPool &exec_pool, x23::ThreadPool &db_pool)
    : exec_pool_(exec_pool), db_pool_(db_pool)
  {}
  // This pool only serves RPCs from the db, which should be zero in this test
  // outside of setup.

  InMemoryDatabase in_memory_db_ = InMemoryDatabase();
  BlockManager block_manager_ = BlockManager(0, &in_memory_db_);

  std::atomic<TransactionID> next_tx_id_ = 0;
  std::unique_ptr<DatabaseProxy> db_proxy_;
  BlockID block_number_ = 0;

  DbExecutorServer db_executor_server_ = DbExecutorServer(&block_manager_);

  std::unique_ptr<ExecutorContext> context_;

  WorkerPool &exec_pool_;
  x23::ThreadPool &db_pool_;
  x23::CoroPoll cp_ = x23::CoroPoll(db_pool_);

  const StorageCostModel cost_model_ = StorageCostModel(12000, 1, 5, 100, 5);

  void initialize()
  {
    constexpr static uint32_t kPortNumber = 10000;
    db_executor_server_.start_server(kPortNumber);

    db_proxy_ =
        std::make_unique<DatabaseProxy>(cp_, LocalMockDB{.read_port_number_ =
                                                             kPortNumber});

    [[maybe_unused]] auto res = block_manager_.add_child(
        0, 1, BlockExecutionMode::kBlockExecutionModeProposal);
    assert(res);
    block_number_++;

    context_ = std::make_unique<ExecutorContext>(ExecutionMode::kProposing,
                                                 block_number_, *db_proxy_,
                                                 exec_pool_);
  }

  void advance_block()
  {
    auto r = block_manager_.get_block_data(block_number_);
    assert(r);
    r->close_block();

    [[maybe_unused]] auto r2 = block_manager_.add_child(
        block_number_, block_number_ + 1,
        BlockExecutionMode::kBlockExecutionModeProposal);
    assert(r2);
    block_number_++;

    context_ = std::make_unique<ExecutorContext>(ExecutionMode::kProposing,
                                                 block_number_, *db_proxy_,
                                                 exec_pool_);
  }

  TransactionID allocate_txid() { return next_tx_id_++; }

  void setup_grant_storage_token(ContractAddress const &address,
                                 int128_t amount)
  {
    assert(amount >= 0);
    assert(amount <= int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

    AddressAndKey addrkey = {
        .address = address,
        .key =
            StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

    in_memory_db_.write_item(
        addrkey,
        std::make_shared<const StorageItemWrapper>(amount, &cost_model_));
  }
};

void
bench_fibonacci(int num_exec_threads, int num_db_threads, int num_txs,
                uint32_t nfib)
{
  WorkerPool exec_pool = WorkerPool(num_exec_threads);
  x23::ThreadPool db_pool = x23::ThreadPool(num_db_threads);

  BenchmarkState bs(exec_pool, db_pool);
  bs.initialize();

  auto contract = test::load_test_contract("fibonacci.wasm");
  ContractAddress contract_addr{};

  bs.in_memory_db_.write_contract(sha256(*contract), contract);
  bs.in_memory_db_.write_deployment(contract_addr, sha256(*contract));

  bs.setup_grant_storage_token(contract_addr, 1'000'000'000);

  using fib_client_t = ContractTraitClient<Fibonacci>;

  fib_client_t fib_client(contract_addr);

  assert(bs.context_);
  auto init = bs.context_->propose_transaction(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                fib_client.make_invocation<"initialize">(
                                    UINT64_MAX)}},
      bs.allocate_txid())();
  if (init.result() != ExecResult::ExecResultOK) {
    throw std::runtime_error("Failed to initialize fibonacci contract");
  }

  bs.advance_block();

  std::vector<SignedTransaction> txs_bench;
  txs_bench.reserve(num_txs);
  for (size_t i = 0; i < num_txs; i++) {
    txs_bench.push_back(SignedTransaction{
        .tx = Transaction{.invocation =
                              fib_client.make_invocation<"slow_fibonacci">(
                                  UINT64_MAX, nfib)}});
  }
  auto const &txs = txs_bench;

  x23::ThreadPool runner_pool(num_exec_threads);

  std::atomic<uint32_t> remaining = num_txs;
  assert(num_txs == txs.size());

  auto [async, cb] = x23::make_async<void>();

  for (auto const &t : txs) {
    [](auto &bs, auto &t, auto &remaining, auto cb,
       auto &runner_pool) -> x23::Detached {
      co_await runner_pool.bg();
      auto res =
          co_await bs.context_->propose_transaction(t, bs.allocate_txid());
      if (res.result() != ExecResult::ExecResultOK) {
        throw std::runtime_error("Failed to propose transaction");
      }
      if (remaining.fetch_sub(1) == 1) {
        cb();
      }
      co_return;
    }(bs, t, remaining, cb, runner_pool);
    async();
  }
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
}
