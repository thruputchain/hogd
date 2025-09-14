#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/bench/common_bench_state.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "transaction.hh"

#include "x23/vec.h"

namespace hogd {

namespace {

struct BenchmarkState : public CommonBenchState {
  BenchmarkState(std::string server_addr, uint16_t exec_port,
                 uint16_t coord_port, uint32_t num_accounts)
    : CommonBenchState(std::move(server_addr), exec_port, coord_port,
                       kControlServerPort),
      num_accounts_(num_accounts)
  {}

  const uint32_t num_accounts_;

  // returns the work transactions
  x23::Vec<SignedTransaction> initialize(uint32_t num_txs)
  {
    HogexecMetrics::launch_log_worker();

    auto exp = ERC20ExperimentState(ERC20PaymentsConfig{
        .num_accounts = num_accounts_,
    });
    auto result = exp.generate_experiment();
    if (!result.has_value()) {
      throw std::runtime_error("Failed to generate erc20 payments experiment");
    }

    auto genesis_block = result.value().genesis_block;
    run_genesis_block(genesis_block, result.value().initialization_txs);

    auto work_txs = exp.extend_experiment(num_txs);
    if (!work_txs.has_value()) {
      throw std::runtime_error("Failed to extend experiment");
    }

    return work_txs.value().work_txs;
  }
};

void
run_bench_rep(BenchmarkState &bs, std::span<SignedTransaction> total_work_txs,
              int num_exec_threads, uint32_t num_txs_per_rep, size_t num_reps)
{
  WorkerPool exec_pool(num_exec_threads);
  exec_pool.adjust_workers(num_exec_threads);
  if (total_work_txs.size() < num_txs_per_rep * num_reps) {
    throw std::runtime_error("not enough transactions to run the benchmark");
  }
  assert((total_work_txs.size() % num_reps) == 0);
  assert(num_txs_per_rep == total_work_txs.size() / num_reps);

  for (size_t nrep = 0U; nrep < num_reps; nrep++) {
    info("bench rep {} for {} txs (accts {}) on {} threads", nrep,
         num_txs_per_rep, bs.num_accounts_, num_exec_threads);
    auto work_txs =
        total_work_txs.subspan(nrep * num_txs_per_rep, num_txs_per_rep);

    auto start_time = std::chrono::steady_clock::now();

    uint32_t ntx_per_thread = num_txs_per_rep / num_exec_threads;
    if (num_txs_per_rep % num_exec_threads != 0) {
      ntx_per_thread++;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(num_exec_threads);

    for (int i = 0; i < num_exec_threads; i++) {
      int offset = i * ntx_per_thread;
      int cur_count = std::min(ntx_per_thread, num_txs_per_rep - (offset));

      futures.emplace_back(
          exec_pool.submit([&bs, work_txs, offset, cur_count]() -> void {
            for (int j = 0; j < cur_count; j++) {

              auto const &tx = work_txs[offset + j];

              auto handler = bs.make_handler(tx);
              if (!handler.root_invoke().is_root_success()) {
                error("Failed to invoke transaction");
                throw std::runtime_error("Failed to invoke transaction");
              }
              if (auto r = handler.try_commit_transaction();
                  !r.has_value() || !r.value()) {
                throw std::runtime_error("failed to perform commit operation");
              }
            }
          }));
    }
    for (auto &f : futures) {
      f.wait();
    }
    auto finish_time = std::chrono::steady_clock::now();
    std::println("time taken: {} us for {} txs (accts {}) on {} threads",
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     finish_time - start_time)
                     .count(),
                 num_txs_per_rep, bs.num_accounts_, num_exec_threads);
    bs.advance_block();
  }
}

void
run_payments_bench(std::string const &server_addr, uint32_t exec_port,
                   uint32_t coord_port, uint32_t num_accounts)
{
  // This is wayyy to slow if we generate and sign all the txs up front.
  // For now, we just reuse the transactions from one rep to the next.
  uint32_t num_total_txs = std::max(100000 * 5, 204800 * 10);
  BenchmarkState bs(server_addr, exec_port, coord_port, num_accounts);
  auto work_txs = bs.initialize(num_total_txs);
  size_t offset = 0;

  // strong scaling
  for (auto nthread : {16, 64, 128, 256, 512, 1024, 2048}) {
    assert(offset + 100000 * 5 <= work_txs.size());
    run_bench_rep(bs,
                  std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                 100000 * 5),
                  nthread, 100000, 5);
    // offset += 10000 * 10;
  }
  // weak scaling
  for (auto nthread : {1, 16, 32, 64, 128, 256, 512, 1024, 2048}) {
    assert(offset + 10000 * 5 <= work_txs.size());
    run_bench_rep(bs,
                  std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                 100 * nthread * 5),
                  nthread, 100 * nthread, 5);
    // offset += 10000 * 10;
  }
}
} // namespace

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -e <exec_port> -c <coord_port> -s <server_addr> -a "
               "<num_accounts>",
               argv[0]);
  exit(EXIT_FAILURE);
}
} // namespace
int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::optional<int> exec_port;
  std::optional<int> coord_port;
  std::optional<std::string> server_addr;
  std::optional<int> num_accounts;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:c:s:a:")) != -1) {
    switch (opt) {
    case 'e':
      exec_port = std::stoi(optarg);
      break;
    case 'c':
      coord_port = std::stoi(optarg);
      break;
    case 's':
      server_addr = std::string(optarg);
      break;
    case 'a':
      num_accounts = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (!server_addr || !exec_port || !coord_port || !num_accounts) {
    usage(argv);
  }

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  hogd::run_payments_bench(*server_addr, *exec_port, *coord_port,
                           *num_accounts);
  // NOLINTEND(bugprone-unchecked-optional-access)

  std::fflush(stdout);
  return 0;
}
