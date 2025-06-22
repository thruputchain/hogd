#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
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
#include "hogd/hogexec/runtime/syscall_handler.h"
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
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {

struct BenchmarkState {

  BenchmarkState(x23::ThreadPool &db_pool) : db_pool_(db_pool) {}
  // This pool only serves RPCs from the db, which should be zero in this test
  // outside of setup.

  InMemoryDatabase in_memory_db_ = InMemoryDatabase();
  BlockManager block_manager_ = BlockManager(0, &in_memory_db_);

  std::atomic<TransactionID> next_tx_id_ = 0;
  BlockID block_number_ = 0;

  DbExecutorServer db_executor_server_ = DbExecutorServer(&block_manager_);

  x23::ThreadPool &db_pool_;
  x23::CoroPoll cp_ = x23::CoroPoll(db_pool_);

  const StorageCostModel cost_model_ = StorageCostModel(12000, 1, 5, 100, 5);

  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;
  std::unique_ptr<DatabaseProxy> db_proxy_;

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

    snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
  }

  SystemCallHandler make_handler(SignedTransaction const &signed_tx)
  {
    auto proxy =
        PerTransactionDatabaseProxy(*db_proxy_, block_number_, allocate_txid());
    return SystemCallHandler(signed_tx, *snapshot_cache_, proxy,
                             ProposalModeData{});
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

    snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
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
setup_benchmark(BenchmarkState &bs)
{
  bs.initialize();

  auto contract = test::load_test_contract("fibonacci.wasm");
  ContractAddress contract_addr{};

  bs.in_memory_db_.write_contract(sha256(*contract), contract);
  bs.in_memory_db_.write_deployment(contract_addr, sha256(*contract));

  bs.setup_grant_storage_token(contract_addr, 1'000'000'000);

  using fib_client_t = ContractTraitClient<Fibonacci>;

  fib_client_t fib_client(contract_addr);

  auto init_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = fib_client.make_invocation<"initialize">(UINT64_MAX)}};

  auto init_handler = bs.make_handler(init_tx);
  if (auto res = init_handler.root_invoke(); !res.is_root_success()) {
    throw std::runtime_error("Failed to initialize fibonacci contract");
  }
  if (auto r = init_handler.try_commit_transaction()();
      !r.has_value() || !r.value()) {
    throw std::runtime_error("Failed to commit transaction");
  }

  bs.advance_block();
}

void
run_bench_rep(BenchmarkState &bs, int num_txs, uint32_t nfib,
              int num_exec_threads)
{ 
  WorkerPool exec_pool(num_exec_threads);
  //auto& exec_pool = WorkerPool::get_default_pool();
  exec_pool.adjust_workers(num_exec_threads);

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t fib_client(ContractAddress{});

  for (size_t nrep = 0; nrep < 10; nrep++) {

    auto start_time = std::chrono::steady_clock::now();
    std::atomic<uint32_t> remaining = num_txs;
    auto [async, cb] = x23::make_async<void>();

    for (size_t i = 0; i < num_txs; i++) {
      exec_pool.submit([&bs, &fib_client, &nfib, &remaining, &cb]() -> void {
        auto tx = SignedTransaction{
            .tx = Transaction{
                .invocation =
                    fib_client.make_invocation<"slow_fibonacci">(UINT64_MAX,
                                                                 nfib)}};
        auto handler = bs.make_handler(tx);
        if (!handler.root_invoke().is_root_success()) {
          throw std::runtime_error("Failed to invoke transaction");
        }
	//if (auto r = handler.try_commit_transaction()(); !r.has_value() || !r.value()) {
	//	throw std::runtime_error("foo");
	//}
        if (remaining.fetch_sub(1) == 1) {
          cb();
        }
      });
    }
    async();
    auto finish_time = std::chrono::steady_clock::now();
    std::println("time taken: {} ms for {} txs (fib {}) on {} threads",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     finish_time - start_time)
                     .count(),
                 num_txs, nfib, num_exec_threads);
  }
}

void
run_fibonacci_wasmonly()
{  // this pool does around 2 rpcs total in this test
  x23::ThreadPool db_pool = x23::ThreadPool(10);

  BenchmarkState bs(db_pool);
  setup_benchmark(bs);

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t fib_client(ContractAddress{});

  for (auto nthread : { 128, 256, 512, 1024, 2048, 4196}) {
    run_bench_rep(bs, 100000, 30, nthread);
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

  hogd::run_fibonacci_wasmonly();
  return 0;
}
