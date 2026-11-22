#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/bench/common_bench_state.h"
#include "hogd/hog_exec/bench/indexer_latency_probe.h"
#include "hogd/hog_exec/bench/mock_scheduler.h"
#include "hogd/hog_exec/bench/multiexec_cluster.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/fastclock.h"
#include "x23/vec.h"

namespace hogd {

namespace {

struct BlockValidationQueue {
  struct Batch {
    size_t start;
    x23::span<const SignedTransaction> txs;
  };

  BlockValidationQueue(const x23::XdrVec<SignedTransaction> *txs,
                       size_t batch_size)
    : txs_(txs->data(), txs->size()), batch_size(batch_size)
  {}

  void reset_offset() { offset.store(0); }

  const std::span<const SignedTransaction> txs_;
  const size_t batch_size;
  std::atomic<uint64_t> offset{0};

  Batch next_batch()
  {
    auto start = offset.fetch_add(batch_size);
    const size_t bounded_start = std::min<size_t>(start, txs_.size());
    return {.start = bounded_start,
            .txs = {txs_.begin() + bounded_start,
                    txs_.begin() +
                        std::min<size_t>(start + batch_size, txs_.size())}};
  }
};

x23::Detached
connection_coro(exec_client_t &exec_client, BlockValidationQueue *queue,
                x23::CoroCB<void> &cb, int ncoro, BlockID block_number,
                std::atomic<size_t> &n_total_coros,
                x23::XdrVec<NondeterministicProposalData> *nominations)
{
  auto coro = [](exec_client_t &exec_client, x23::CoroCB<void> &cb,
                 BlockValidationQueue *queue, BlockID block_number,
                 std::atomic<size_t> &n_total_coros,
                 x23::XdrVec<NondeterministicProposalData> *nominations)
      -> x23::Detached {
    co_await default_threadpool().bg();
    while (true) {
      auto batch = queue->next_batch();
      if (batch.txs.empty()) {
        if (n_total_coros.fetch_sub(1, std::memory_order_relaxed) == 1) {
          cb();
        }
        co_return;
      }

      auto r = co_await exec_client.executorsched_propose_batch(block_number,
                                                                batch.txs);
      assert(r.size() == batch.txs.size());
      for (size_t i = 0; i < r.size(); ++i) {
        const ExecProposeResult &result = r[i];
        if (result.result() != ExecResult::ExecResultOK) {
          panic("Failed to propose batch");
        }
        (*nominations)[batch.start + i] = result.proposal_data();
      }
    }
  };

  for (int i = 0; i < ncoro; i++) {
    coro(exec_client, cb, queue, block_number, n_total_coros, nominations);
  }
  co_return;
};

bool
event_storage_experiment_branch()
{
  const char *branch = std::getenv("GIT_BRANCH");
  const char *container = std::getenv("HOGD_CI_CONTAINER");
  return branch && std::string_view(branch) == "codex/multiexec-event-storage" &&
         container && std::string_view(container) == "1";
}

bool
close_diagnostics_enabled()
{
  const char *diagnostics = std::getenv("HOGD_MULTIEXEC_CLOSE_DIAGNOSTICS");
  const char *branch = std::getenv("GIT_BRANCH");
  return event_storage_experiment_branch() ||
         (diagnostics && std::string_view(diagnostics) == "1") ||
         (branch && std::string_view(branch) ==
                        "codex/multiexec-close-diagnostics");
}

std::FILE *
open_close_diagnostics(size_t executors, size_t batch_size)
{
  const char *dump_dir = std::getenv("HOG_DUMP_DIR");
  if (!dump_dir || !*dump_dir) {
    return nullptr;
  }
  std::error_code error;
  std::filesystem::create_directories(dump_dir, error);
  if (error) {
    warn("Cannot create multiexec diagnostics directory {}: {}", dump_dir,
         error.message());
    return nullptr;
  }
  const auto path = std::filesystem::path(dump_dir) /
                    std::format("multiexec-close-{}exec-batch{}.log", executors,
                                batch_size);
  auto *file = std::fopen(path.c_str(), "a");
  if (!file) {
    warn("Cannot open multiexec diagnostics file {}", path.string());
  }
  return file;
}

void
run_block(x23::XdrVec<SignedTransaction> const *txs,
          bench_db_sched_client_t &db_sched_client,
          bench_event_sched_client_t &event_sched_client,
          std::vector<exec_client_t> &exec_clients,
          size_t n_coros_per_connection, size_t n_connections,
          size_t batch_size, uint64_t parent_block_number,
          uint64_t current_block_number, ConsensusValue &prev_value,
          IndexerLatencyProbe *indexer_probe, size_t diagnostic_executor_count,
          std::string *diagnostic_records)
{
  auto start_time = x23::fastclock::now();

  std::atomic<size_t> n_total_coros = n_coros_per_connection * n_connections;
  // Height comes off the previous committed value, never off a block number.
  open_block(db_sched_client, event_sched_client, exec_clients,
             parent_block_number, current_block_number,
             prev_value.proposal_header.block_height + 1);

  BlockValidationQueue block_validation_queue(txs, batch_size);
  x23::XdrVec<NondeterministicProposalData> nominations(txs->size());

  auto start_coros_time = x23::fastclock::now();

  // make sure any destructors are run during the measured inner loop
  {
    auto [async, cb] = x23::make_async<void>();
    for (auto &exec_client : exec_clients) {
      connection_coro(exec_client, &block_validation_queue, cb,
                      n_coros_per_connection, current_block_number,
                      n_total_coros, &nominations);
    }
    async();
  }

  auto end_coros_time = x23::fastclock::now();
  double elapsed_time = (end_coros_time - start_coros_time).count();
  auto inner_tps = txs->size() * 1'000'000'000.0 / elapsed_time;
  if (diagnostic_executor_count != 0) {
    close_block<true>(db_sched_client, event_sched_client, exec_clients,
                       current_block_number, prev_value, indexer_probe,
                       diagnostic_executor_count, diagnostic_records);
  }
  else {
    close_block(db_sched_client, event_sched_client, exec_clients,
                current_block_number, prev_value, indexer_probe);
  }

  auto end_block_time = x23::fastclock::now();
  std::println("open_time {} middle_time {} close_time {} inner tps: {}",
               (start_coros_time - start_time).count(),
               (end_coros_time - start_coros_time).count(),
               (end_block_time - end_coros_time).count(), inner_tps);
}

void
run_multiexec_bench(const std::vector<std::string> &server_addrs,
                    unsigned int num_accounts,
                    unsigned int n_connections_per_executor,
                    unsigned int n_coros_per_connection,
                    unsigned int batch_size, unsigned int num_txs_per_block,
                    const std::string &event_work_dir = {})
{
  MultiexecCluster cluster(server_addrs, event_work_dir);

  IndexerLatencyProbe indexer_probe(cluster.indexer_block_explorer_addr());

  const size_t diagnostic_executor_count =
      close_diagnostics_enabled() ? cluster.num_executors() : 0;
  std::unique_ptr<std::FILE, decltype(&std::fclose)> diagnostic_file(
      diagnostic_executor_count
          ? open_close_diagnostics(diagnostic_executor_count, batch_size)
          : nullptr,
      &std::fclose);
  std::string recorded_close_callbacks;
  auto *diagnostic_records = &recorded_close_callbacks;

  auto &event_sched_client = cluster.event_sched_client();
  auto &db_sched_client = cluster.db_sched_client();

  uint64_t parent_block_number = 0;
  uint64_t current_block_number = 1;
  ConsensusValue prev_value = MockSchedEvent::bench_genesis_tip();

  ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
  config.erc20_payments() = {
      .num_addresses = num_accounts,
      .network_hash = {},
  };

  LoadgenRpc loadgen;
  auto loadgen_result = loadgen.generate_experiment(config)();
  if (loadgen_result.error() != LoadgenRPCError::kLoadgenRPCErrorOK) {
    panic("Failed to generate loadgen experiment");
  }
  auto genesis_configuration = loadgen_result.success().genesis_configuration;
  cluster.write_genesis_configuration(genesis_configuration);

  const size_t n_connections =
      n_connections_per_executor * cluster.num_executors();
  std::vector<exec_client_t> coro_exec_clients =
      cluster.connect_exec_clients(n_connections_per_executor);

  run_block(&loadgen_result.success().genesis_ceremony_txs, db_sched_client,
            event_sched_client, coro_exec_clients, n_coros_per_connection,
            n_connections, batch_size, parent_block_number,
            current_block_number, prev_value, &indexer_probe,
            diagnostic_executor_count, diagnostic_records);
  parent_block_number = current_block_number;
  current_block_number++;

  for (auto const &init_block : loadgen_result.success().initialization_txs) {
    run_block(&init_block, db_sched_client, event_sched_client,
              coro_exec_clients, n_coros_per_connection, n_connections,
              batch_size, parent_block_number, current_block_number, prev_value,
              &indexer_probe, diagnostic_executor_count, diagnostic_records);
    parent_block_number = current_block_number;
    current_block_number++;
  }

  info("done initialization txs");

  auto work_txs_raw =
      loadgen.extend_experiment(loadgen_result.success().context_id,
                                num_txs_per_block, 1000000)();
  if (work_txs_raw.error() != LoadgenRPCError::kLoadgenRPCErrorOK) {
    panic("Failed to extend loadgen experiment");
  }
  SignedTransactionBatch work_txs;
  parallel_batch_to_t(work_txs_raw.success().work_txs, work_txs,
                      kNoBatchSizeLimit)();
  CHECK_EQ(work_txs.size(), num_txs_per_block);
  if (!event_work_dir.empty()) {
    SHA256Hasher hasher;
    for (const auto &tx : work_txs) {
      hasher.update(x23::serialize(tx));
    }
    auto hash = hasher.finalize();
    std::println("multiexec_storage_workload_sha256={}",
                 encode_hex(std::as_bytes(std::span(hash))));
  }

  // The same work txs are replayed across every trial, so each must carry a
  // fresh uniquifier (HogDb now rejects a re-consumed one). The signature
  // covers only the inner calldata, not the uniquifier, so we can rewrite it in
  // place. A global index across trials keeps nonces unique and each
  // expiry-height under the replay-cache per-height cap; expiry heights start
  // at this block, so none are expired when committed.
  //
  // Trial i's lowest expiry height is work_base + floor(i * work_txs.size() /
  // kBenchUniquifiersPerHeight) and it commits at work_base + i, so we need
  // work_txs.size() >= kBenchUniquifiersPerHeight or trial 1 would assign an
  // already-expired height.
  CHECK_GE(work_txs.size(), CommonBenchState::kBenchUniquifiersPerHeight);
  const BlockID work_base = current_block_number;
  for (size_t i = 0; i < 5; i++) {
    for (size_t j = 0; j < work_txs.size(); j++) {
      work_txs[j].tx.uniquifier =
          CommonBenchState::bench_uniquifier(work_base,
                                             (i * work_txs.size()) + j);
    }
    std::println("Start trial {} with txs={} nexecutors={} nconnections={} "
                 "ncoros_per_connection={} nacc={} "
                 "batch_size={}",
                 i, num_txs_per_block, cluster.num_executors(), n_connections,
                 n_coros_per_connection, num_accounts, batch_size);
    auto ts = x23::fastclock::now();
    run_block(&work_txs, db_sched_client, event_sched_client, coro_exec_clients,
              n_coros_per_connection, n_connections, batch_size,
              parent_block_number, current_block_number, prev_value,
              &indexer_probe, diagnostic_executor_count, diagnostic_records);
    auto end_time = x23::fastclock::now();
    double elapsed_time = (end_time - ts).count();
    auto tps = num_txs_per_block * 1'000'000'000.0 / elapsed_time;
    std::println("time taken: {} ns for txs={} nexecutors={} nconnections={} "
                 "ncoros_per_connection={} nacc={} "
                 "batch_size={} tps={}",
                 elapsed_time, num_txs_per_block, cluster.num_executors(),
                 n_connections, n_coros_per_connection, num_accounts,
                 batch_size, tps);
    info("time taken: {} ns for txs={} nexecutors={} nconnections={} "
         "ncoros_per_connection={} nacc={} "
         "batch_size={} tps={}",
         elapsed_time, num_txs_per_block, cluster.num_executors(),
         n_connections, n_coros_per_connection, num_accounts, batch_size, tps);
    parent_block_number = current_block_number;
    current_block_number++;
  }

  if (!recorded_close_callbacks.empty()) {
    auto *output = diagnostic_file ? diagnostic_file.get() : stdout;
    if (std::fputs(recorded_close_callbacks.c_str(), output) == EOF ||
        std::fflush(output) != 0) {
      warn("Failed to write multiexec close diagnostics");
      if (diagnostic_file) {
        std::fputs(recorded_close_callbacks.c_str(), stdout);
      }
    }
  }

  // Every block of this run has committed; let the indexer finish draining
  // before reporting. The trials commit far faster than the indexer ingests,
  // so the backlog standing here outlives the run by many seconds and the
  // drain bounds the indexer going quiet rather than the total wait. A stall
  // is reported as [unobserved] rather than silently shrinking the sample.
  if (!indexer_probe.drain()) {
    warn("indexer latency probe: indexer stopped draining committed heights");
  }
  indexer_probe.stop();
  indexer_probe.report(
      std::format("nexec={},batch={}", cluster.num_executors(), batch_size));
}

} // namespace

} // namespace hogd

namespace {
[[noreturn]] void
usage(char *argv[])
{
  std::println("Usage: {} -s <server_addr, repeated> -a "
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

  std::vector<std::string> server_addrs;
  std::optional<int> num_accounts;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "s:a:")) != -1) {
    switch (opt) {
    case 's':
      server_addrs.emplace_back(optarg);
      break;
    case 'a':
      num_accounts = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (server_addrs.empty() || !num_accounts) {
    usage(argv);
  }

  const int account_count = num_accounts.value();
  if (hogd::event_storage_experiment_branch()) {
    const char *directory = std::getenv("HOGD_MULTIEXEC_EVENT_ARM_DIR");
    const std::vector<std::string> expected_hosts{
        "hog11", "hog8", "hog4", "hog5", "hog7", "hog6"};
    CHECK(directory && server_addrs == expected_hosts && account_count == 100);
    auto suffix = std::string_view(directory);
    CHECK(suffix.starts_with("/bench-event/multiexec-event-storage.") ||
          suffix.starts_with("/workspace/multiexec-event-storage."));
    suffix.remove_prefix(suffix.starts_with("/bench-event/") ?
                             std::string_view("/bench-event/").size() :
                             std::string_view("/workspace/").size());
    suffix.remove_prefix(std::string_view("multiexec-event-storage.").size());
    CHECK(suffix.size() == 12 &&
          suffix.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz") ==
              std::string_view::npos);
    hogd::run_multiexec_bench(server_addrs, account_count, 10, 100, 10,
                              1'000'000, directory);
    std::fflush(stdout);
    return 0;
  }
  hogd::run_multiexec_bench(server_addrs, account_count, 10, 100, 1, 1'000'000);
  hogd::run_multiexec_bench(server_addrs, account_count, 10, 100, 5, 1'000'000);
  hogd::run_multiexec_bench(server_addrs, account_count, 10, 100, 10,
                            1'000'000);

  std::fflush(stdout);
  return 0;
}
