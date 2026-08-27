#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ci_server.hh"
#include "consensus.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/bench/common_bench_state.h"
#include "hogd/hog_exec/bench/control_client.h"
#include "hogd/hog_exec/bench/mock_scheduler.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "loadgen.hh"
#include "network.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/fastclock.h"
#include "x23/format.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

namespace {

class SchedExecTest {
public:
  using rpc_service = SchedExec1;

  SchedExecTest() = default;

  // Disable copy and move.
  SchedExecTest(const SchedExecTest &) = delete;
  SchedExecTest &operator=(const SchedExecTest &) = delete;
  SchedExecTest(SchedExecTest &&) = delete;
  SchedExecTest &operator=(SchedExecTest &&) = delete;

  x23::Async<IntroExecResult> schedexec_intro_exec(const ExecCard &card)
  {
    info("executor with card {} connected", x23::show(card));

    auto r = default_coropoll().pool_.async(
        [](const ExecCard &card, std::mutex &mtx, std::condition_variable &cv,
           std::vector<ExecCard> &exec_cards, uint16_t &exec_id,
           const EventCard &event_card,
           const x23::Vec<ShardCard> &shard_cards) {
          info("doing lambda for schedexec_intro_exec");
          std::unique_lock lk(mtx);
          exec_cards.push_back(card);
          info("pushed card {} to exec_cards", exec_cards.size());
          cv.notify_all();

          cv.wait(lk, [&event_card] { return event_card != EventCard{}; });
          info("event_card is not empty");
          cv.wait(lk, [&shard_cards] { return !shard_cards.empty(); });
          info("shard_cards is not empty: returning IntroExecResult");

          return IntroExecResult{.exec_id = exec_id++,
                                 .shards = x23::XdrVec<ShardCard>{shard_cards},
                                 .event = event_card};
        },
        card, std::ref(mtx_), std::ref(cv_), std::ref(exec_cards_),
        std::ref(exec_id_), std::ref(event_card_), std::ref(shard_cards_));

    co_return co_await r;
  }

  void register_event_card(EventCard card)
  {
    std::unique_lock lk(mtx_);
    event_card_ = std::move(card);
    cv_.notify_all();
  }

  void register_shard_cards(x23::Vec<ShardCard> cards)
  {
    std::unique_lock lk(mtx_);
    shard_cards_ = std::move(cards);
    cv_.notify_all();
  }

  std::vector<ExecCard> await_executor_cards(unsigned int num_cards)
  {
    info("awaiting {} executor cards", num_cards);
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this, num_cards] { return exec_cards_.size() == num_cards; });
    info("got {} executor cards", exec_cards_.size());
    return std::move(exec_cards_);
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<ExecCard> exec_cards_;
  uint16_t exec_id_ = 0;
  EventCard event_card_{};
  x23::Vec<ShardCard> shard_cards_;
};

static_assert(is_SchedExec1<SchedExecTest *, x23::MaybeAsync>);

auto
connect_to_event_sched_server(const ServerAddress &address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    panic("Failed to parse event sched server address");
  }
  auto client =
      make_rpc_client<EventSched1>(host_port, false,
                                   kEventSchedulerRpcMaxMessageSize)();
  if (!client)
    panic("connect_to_event_sched_server failed: {}",
          client.error().error_code().message());
  return std::move(*client);
}

auto
connect_to_db_server(ShardCard const &card)
{
  x23::HostPort host_port;
  if (!host_port.parse(card.address_for_sched)) {
    panic("Failed to parse db server address");
  }
  return make_srpc_client<DbSched1>(host_port, false);
}

auto
connect_to_exec_server(const ExecCard &card)
{
  x23::HostPort host_port;
  if (!host_port.parse(card.address)) {
    panic("Failed to parse exec server address");
  }
  auto client = make_rpc_client<ExecutorSched1>(host_port, false)();
  if (!client)
    panic("connect_to_exec_server failed: {}",
          client.error().error_code().message());
  return std::move(*client);
}

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

using exec_client_t =
    ExecutorSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;
using event_sched_client_t = decltype(connect_to_event_sched_server("address"));
using db_sched_client_t = decltype(connect_to_db_server(ShardCard{}));

void
open_block(db_sched_client_t &db_sched_client,
           event_sched_client_t &event_sched_client,
           std::vector<exec_client_t> &exec_clients,
           uint64_t parent_block_number, uint64_t block_number)
{
  info("opening block {}", block_number);
  auto start_time = x23::fastclock::get_ns();

  auto [async, cb] = x23::make_async<void>();
  std::atomic<size_t> n_remaining_execs = exec_clients.size();

  for (auto &exec_client : exec_clients) {
    [](auto &exec_client, uint64_t block_number,
       std::atomic<size_t> &n_remaining_execs, auto &cb) -> x23::Detached {
      co_await exec_client.executorsched_open_proposal_block(block_number);
      if (n_remaining_execs.fetch_sub(1, std::memory_order_relaxed) == 1) {
        cb();
      }
    }(exec_client, block_number, n_remaining_execs, cb);
  }

  auto event_async = event_sched_client.eventsched_open_block(block_number);
  auto issue_time = x23::fastclock::get_ns();
  auto res = db_sched_client.dbsched_create_snapshot(
      parent_block_number, block_number,
      BlockExecutionMode::kBlockExecutionModeProposal);
  if (res != DbSchedResult::DbSchedResultOK) {
    panic("Failed to create snapshot");
  }
  auto create_time = x23::fastclock::get_ns();
  async();
  event_async();
  auto done_time = x23::fastclock::get_ns();

  std::println("opening: issue_time {} create_time {} done_time {}",
               issue_time - start_time, create_time - issue_time,
               done_time - create_time);
};

// [prev_value] chains the committed ConsensusValues: it enters holding the
// previous block's committed value (the mock scheduler's genesis tip before
// the first block) and leaves holding this block's.
void
close_block(db_sched_client_t &db_sched_client,
            event_sched_client_t &event_sched_client,
            std::vector<exec_client_t> &exec_clients, uint64_t block_number,
            ConsensusValue &prev_value)
{
  auto start_time = x23::fastclock::get_ns();
  info("closing block {}", block_number);
  auto [async, cb] = x23::make_async<void>();
  std::atomic<size_t> n_remaining_execs = exec_clients.size();

  for (auto &exec_client : exec_clients) {
    [](auto &exec_client, uint64_t block_number,
       std::atomic<size_t> &n_remaining_execs, auto &cb) -> x23::Detached {
      co_await exec_client.executorsched_finish_block(block_number);
      if (n_remaining_execs.fetch_sub(1, std::memory_order_relaxed) == 1) {
        cb();
      }
    }(exec_client, block_number, n_remaining_execs, cb);
  }
  // We use [block_number] as the height as well.
  auto event_async =
      event_sched_client.eventsched_close_block(block_number, block_number);
  auto issue_time = x23::fastclock::get_ns();
  auto res = db_sched_client.dbsched_close_snapshot(block_number);
  if (res.result() != DbSchedResult::DbSchedResultOK) {
    panic("Failed to close snapshot");
  }
  auto close_time = x23::fastclock::get_ns();
  ConsensusValue value{};
  value.proposal_header.block_height = block_number;
  auto commit_res =
      db_sched_client.dbsched_commit_snapshot(block_number, value);
  if (commit_res.result() != DbSchedResult::DbSchedResultOK) {
    panic("Failed to commit snapshot");
  }
  auto commit_time = x23::fastclock::get_ns();
  // It's ok for finish_block to overlap with db.close_block, because we already
  // synchronized waiting for all pending txs on the block to finish.
  async();
  auto event_result = event_async();
  if (event_result.error() != RPCError::kRPCNone) {
    panic("Failed to close event block");
  }
  auto done_time = x23::fastclock::get_ns();
  // Commit the event block the way a real finalization does, so the measured
  // lifecycle covers the durable indexer publication and not just close. The
  // finalized value chains prev_hash off the previous committed value and
  // carries the event root close returned; the event server validated its
  // origin against the mock scheduler's genesis tip at registration.
  ConsensusValue event_value{};
  event_value.prev_hash = sha256(prev_value);
  event_value.proposal_header.block_height =
      static_cast<BlockHeight>(block_number);
  event_value.proposal_header.events_hash = event_result.root_hash();
  auto event_commit_res =
      event_sched_client.eventsched_commit_block(block_number, event_value)();
  if (event_commit_res != RPCError::kRPCNone) {
    panic("Failed to commit event block");
  }
  prev_value = event_value;
  auto event_commit_time = x23::fastclock::get_ns();
  std::println("closing: issue_time {} close_time {} commit_time {} "
               "done_time {} event_commit_time {}",
               issue_time - start_time, close_time - issue_time,
               commit_time - close_time, done_time - commit_time,
               event_commit_time - done_time);
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

void
run_block(x23::XdrVec<SignedTransaction> const *txs,
          db_sched_client_t &db_sched_client,
          event_sched_client_t &event_sched_client,
          std::vector<exec_client_t> &exec_clients,
          size_t n_coros_per_connection, size_t n_connections,
          size_t batch_size, uint64_t parent_block_number,
          uint64_t current_block_number, ConsensusValue &prev_value)
{
  auto start_time = x23::fastclock::get_ns();

  std::atomic<size_t> n_total_coros = n_coros_per_connection * n_connections;
  open_block(db_sched_client, event_sched_client, exec_clients,
             parent_block_number, current_block_number);

  BlockValidationQueue block_validation_queue(txs, batch_size);
  x23::XdrVec<NondeterministicProposalData> nominations(txs->size());

  auto start_coros_time = x23::fastclock::get_ns();

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

  auto end_coros_time = x23::fastclock::get_ns();
  double elapsed_time = end_coros_time - start_coros_time;
  auto inner_tps = txs->size() * 1'000'000'000.0 / elapsed_time;
  close_block(db_sched_client, event_sched_client, exec_clients,
              current_block_number, prev_value);

  auto end_block_time = x23::fastclock::get_ns();
  std::println("open_time {} middle_time {} close_time {} inner tps: {}",
               start_coros_time - start_time, end_coros_time - start_coros_time,
               end_block_time - end_coros_time, inner_tps);
}

void
run_multiexec_bench(const std::vector<std::string> &server_addrs,
                    unsigned int num_accounts,
                    unsigned int n_connections_per_executor,
                    unsigned int n_coros_per_connection,
                    unsigned int batch_size, unsigned int num_txs_per_block)
{
  std::vector<ControlServerHandle> control_server_handles;
  control_server_handles.reserve(server_addrs.size());
  for (const auto &server_addr : server_addrs) {
    control_server_handles.emplace_back(server_addr);
  }

  if (server_addrs.size() < 3) {
    // 1 db, 1 event server, and the rest executors
    panic("At least 3 servers are required");
  }

  RpcServer<SchedExecTest> sched_exec_test;
  uint16_t sched_exec_port = sched_exec_test.start_server(0);

  RpcServer<MockSchedDB> sched_db_test;
  uint16_t sched_db_port = sched_db_test.start_server(0);

  RpcServer<MockSchedEvent> sched_event_test;
  uint16_t sched_event_port = sched_event_test.start_server(0);

  auto work_dir = control_server_handles[0].ci_client.query_work_dir();
  if (work_dir.error_code() != CIServerErrorCode::kCIServerErrorCodeOK) {
    error("Failed to query work dir: {}", work_dir.error_code());
    panic("Failed to query work dir");
  }

  std::string work_dir_str(work_dir.work_dir().begin(),
                           work_dir.work_dir().end());
  info("using work dir: {}", work_dir_str);

  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  std::string mempool_ip = hog_ip(hostname);

  for (auto &control_server_handle : control_server_handles) {
    if (control_server_handle.ci_client.clear_work_dir() !=
        CIServerErrorCode::kCIServerErrorCodeOK) {
      panic("Failed to clear work dir");
    }
  }

  size_t server_idx = 0;
  control_server_handles[server_idx].ci_client.start_server(
      "hogd/hog_event/hog-event",
      {"--bind-ip", hog_ip(server_addrs[server_idx]), "--mempool-addr",
       std::format("{}:{}", mempool_ip, sched_event_port), "--work-dir",
       work_dir_str});
  server_idx++;

  EventCard event_card = sched_event_test->impl()->await_event_card();
  sched_exec_test->impl()->register_event_card(event_card);

  control_server_handles[server_idx].ci_client.start_server(
      "hogd/hog_db/hog-db",
      {"--db-path", work_dir_str, "--ram-bytes", "10000000000", "--bind-ip",
       hog_ip(server_addrs[server_idx]), "--coordinator-addr",
       std::format("{}:{}", mempool_ip, sched_db_port)});
  server_idx++;

  for (; server_idx < server_addrs.size(); server_idx++) {
    control_server_handles[server_idx].ci_client.start_server(
        "hogd/hog_exec/hog-exec",
        {"--bind-ip", hog_ip(server_addrs[server_idx]), "--coordinator-addr",
         std::format("{}:{}", mempool_ip, sched_exec_port), "-n", "1024"});
  }

  auto db_cards = sched_db_test->impl()->await_shard_cards(1);
  sched_exec_test->impl()->register_shard_cards(db_cards);
  auto exec_cards =
      sched_exec_test->impl()->await_executor_cards(server_addrs.size() - 2);

  auto event_sched_client =
      connect_to_event_sched_server(event_card.address_for_sched);
  assert(db_cards.size() == 1);
  auto db_sched_client = connect_to_db_server(db_cards[0]);

  auto await_ready = [&](const ExecCard &exec_card) {
    auto exec_client = connect_to_exec_server(exec_card);
    while (true) {
      auto res = exec_client.executorsched_ready()();
      if (res != ExecResult::ExecResultOK) {
        warn_every_n_sec(1.0, "Executor is not ready, retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      break;
    }
  };
  for (auto &exec_card : exec_cards) {
    await_ready(exec_card);
  }

  auto write_genesis_configuration =
      [&](const GenesisConfiguration &genesis_configuration) {
        ConsensusValue genesis_value{};
        genesis_value.proposal_header.block_height = 0;
        genesis_value.proposal_header.config_hash =
            compute_config_hash(genesis_configuration.storage_cost_model);
        auto res = db_sched_client.dbsched_load_genesis_configuration(
            genesis_configuration, genesis_value);
        if (res != DbSchedResult::DbSchedResultOK) {
          panic("Failed to write genesis block");
        }
      };

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
  write_genesis_configuration(genesis_configuration);

  const size_t n_connections = n_connections_per_executor * exec_cards.size();

  std::vector<exec_client_t> coro_exec_clients;
  coro_exec_clients.reserve(n_connections);
  for (size_t i = 0; i < n_connections; i++) {
    coro_exec_clients.push_back(
        connect_to_exec_server(exec_cards[i % exec_cards.size()]));
  }

  run_block(&loadgen_result.success().genesis_ceremony_txs, db_sched_client,
            event_sched_client, coro_exec_clients, n_coros_per_connection,
            n_connections, batch_size, parent_block_number,
            current_block_number, prev_value);
  parent_block_number = current_block_number;
  current_block_number++;

  for (auto const &init_block : loadgen_result.success().initialization_txs) {
    run_block(&init_block, db_sched_client, event_sched_client,
              coro_exec_clients, n_coros_per_connection, n_connections,
              batch_size, parent_block_number, current_block_number,
              prev_value);
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
  parallel_batch_to_t(work_txs_raw.success().work_txs, work_txs)();
  CHECK_EQ(work_txs.size(), num_txs_per_block);

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
                 i, num_txs_per_block, exec_cards.size(), n_connections,
                 n_coros_per_connection, num_accounts, batch_size);
    auto ts = x23::fastclock::get_ns();
    run_block(&work_txs, db_sched_client, event_sched_client, coro_exec_clients,
              n_coros_per_connection, n_connections, batch_size,
              parent_block_number, current_block_number, prev_value);
    auto end_time = x23::fastclock::get_ns();
    double elapsed_time = end_time - ts;
    auto tps = num_txs_per_block * 1'000'000'000.0 / elapsed_time;
    std::println("time taken: {} ns for txs={} nexecutors={} nconnections={} "
                 "ncoros_per_connection={} nacc={} "
                 "batch_size={} tps={}",
                 elapsed_time, num_txs_per_block, exec_cards.size(),
                 n_connections, n_coros_per_connection, num_accounts,
                 batch_size, tps);
    info("time taken: {} ns for txs={} nexecutors={} nconnections={} "
         "ncoros_per_connection={} nacc={} "
         "batch_size={} tps={}",
         elapsed_time, num_txs_per_block, exec_cards.size(), n_connections,
         n_coros_per_connection, num_accounts, batch_size, tps);
    parent_block_number = current_block_number;
    current_block_number++;
  }
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
  hogd::run_multiexec_bench(server_addrs, account_count, 10, 100, 1, 1'000'000);
  hogd::run_multiexec_bench(server_addrs, account_count, 10, 100, 5, 1'000'000);
  hogd::run_multiexec_bench(server_addrs, account_count, 10, 100, 10,
                            1'000'000);

  std::fflush(stdout);
  return 0;
}
