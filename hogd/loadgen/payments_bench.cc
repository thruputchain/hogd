#include <benchmark/benchmark.h>

#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/coord/coord_block_manager.h"
#include "hogd/coord/coord_core.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/hogexec_core.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/init.h"
#include "hogd/loadgen/payment_experiment.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/rpc.h"

namespace hogd {

namespace {

void
bm_payments(benchmark::State &state)
{
  constexpr static size_t kBlocks = 10;
  constexpr static size_t kBlockSize = 1000;

  const std::string db_to_exec_port = std::to_string(kDBToExecutorPortNum);
  const std::string db_to_coord_port = std::to_string(kDBToCoordPortNum);
  const std::string exec_to_coord_port = std::to_string(kExecToCoordPortNum);
  const std::string coord_announce_port = std::to_string(kCoordAnnouncePortNum);

  const unsigned int nexec_workthreads = state.range(0);
  const unsigned int nexec_corothreads = 1 + (nexec_workthreads / 4);
  const unsigned int ndb_corothreads = 1; // nexec_corothreads;

  std::mutex mtx;
  std::condition_variable cv;
  bool test_done = false;
  bool executor_connected = false;

  std::thread db_thread = std::thread([&] {
    HogdbCore db_core(ndb_corothreads, db_to_exec_port, db_to_coord_port);
    db_core.get_root_db().set_as_default_genesis();

    db_core.announce_to_coordinator("localhost", coord_announce_port);

    std::unique_lock lk(mtx);
    if (!test_done) {
      cv.wait(lk, [&] { return test_done; });
    }
  });
  std::thread exec_thread = std::thread([&] {
    HogexecCore exec_core =
        start_hogexec(nexec_corothreads, nexec_workthreads, exec_to_coord_port,
                      "localhost", coord_announce_port);
    exec_core.announce_ready_to_coord("localhost", coord_announce_port);

    {
      std::unique_lock lk(mtx);
      executor_connected = true;
      cv.notify_all();
    }

    exec_core.await_shutdown();
  });

  std::unique_ptr<CoordinatorCore> coord = std::make_unique<CoordinatorCore>(1);

  coord->start(coord_announce_port);
  {
    std::unique_lock lk(mtx);
    if (!executor_connected) {
      cv.wait(lk, [&] { return executor_connected; });
    }
  }

  size_t naccs = state.range(1);

  PaymentExperiment experiment(naccs);

  auto init_blocks = experiment.produce_initialization_transactions();

  SHA256Hash parent_hash = CoordinatorBlockManager::kGenesisBlockHash;

  for (auto const &block : init_blocks) {
    SHA256Hash block_hash = sha256(parent_hash);
    auto res = coord->validate_block(parent_hash, block, block_hash)();
    if (!res) {
      throw std::runtime_error("failed to initialize benchmark state");
    }
    parent_hash = block_hash;
  }
  // if google benchmark wants additional blocks beyond the 100
  // then for now we just loop through these, since we haven't impl'd a replay
  // prevention mechanism yet and we don't want to benchmark ed25519_sign in the
  // core loop
  std::vector<std::vector<ProposedTransaction>> test_blocks;
  test_blocks.reserve(kBlocks);
  for (size_t i = 0; i < kBlocks; i++) {
    test_blocks.emplace_back(
        experiment.make_random_pairwise_payments(kBlockSize));
  }

  size_t block_counter = 0;
  for (auto _ : state) {
    auto &block = test_blocks[block_counter];
    block_counter++;
    block_counter = block_counter % test_blocks.size();

    SHA256Hash block_hash = sha256(parent_hash);

    if (!coord->validate_block(parent_hash, block, block_hash)()) {
      throw std::runtime_error("validation failed");
    }
    parent_hash = block_hash;
  }
  coord.reset();

  {
    std::lock_guard lock(mtx);
    test_done = true;
    cv.notify_all();
  }
  db_thread.join();
  exec_thread.join();
}
BENCHMARK(bm_payments)->ArgsProduct({{1, 2, 4, 8}, {2, 10, 100, 1000}});

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  using hogd::GStatus;
  GStatus<void> ret = hogd::crypto_init();
  if (!ret)
    return EXIT_FAILURE;
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
