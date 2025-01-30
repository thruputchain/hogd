#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "hogd/coord/coord_core.h"
#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/hogexec_core.h"
#include "hogd/loadgen/payment_experiment.h"
#include "rpc_types.hh"

namespace hogd {

class PaymentExperimentTest : public ::testing::Test {
protected:
  const std::string kDBToExecutorPort = std::to_string(kDBToExecutorPortNum);
  const std::string kDBToCoordPort = std::to_string(kDBToCoordPortNum);
  const std::string kExecToCoordPort = std::to_string(kExecToCoordPortNum);
  const std::string kCoordAnnouncePort = std::to_string(kCoordAnnouncePortNum);

  std::mutex mtx_;
  std::condition_variable cv_;
  bool test_done_ = false;
  bool executor_connected_ = false;

  std::thread db_thread_;
  std::thread exec_thread_;

  void SetUp() override
  {
    db_thread_ = std::thread([&] {
      HogdbCore db_core(1, kDBToExecutorPort, kDBToCoordPort);
      db_core.announce_to_coordinator("localhost", kCoordAnnouncePort);

      db_core.get_root_db().set_as_default_genesis();

      std::unique_lock lk(mtx_);
      if (test_done_) {
        return;
      }
      cv_.wait(lk, [&] { return test_done_; });
    });
    exec_thread_ = std::thread([&] {
      HogexecCore exec_core =
          start_hogexec(1, kExecToCoordPort, "localhost", kCoordAnnouncePort);
      exec_core.announce_ready_to_coord("localhost", kCoordAnnouncePort);

      std::unique_lock lk(mtx_);
      std::printf("executor connected\n");
      executor_connected_ = true;
      cv_.notify_all();
      if (test_done_) {
        std::printf("executor done\n");
        return;
      }
      cv_.wait(lk, [&] { return test_done_; });
      std::printf("executor done\n");
    });

    coord_ = std::make_unique<CoordinatorCore>(1);

    coord_->start(kCoordAnnouncePort);
    {
      std::unique_lock lk(mtx_);
      if (!executor_connected_) {
        cv_.wait(lk, [&] { return executor_connected_; });
      }
    }
  }

  void TearDown() override
  {
    test_done_ = true;
    cv_.notify_all();
    db_thread_.join();
    exec_thread_.join();
  }

  std::unique_ptr<CoordinatorCore> coord_;
};

TEST_F(PaymentExperimentTest, Basic)
{
  PaymentExperiment experiment(2);

  auto init_blocks = experiment.produce_initialization_transactions();

  SHA256Hash parent_hash = CoordinatorBlockManager::kGenesisBlockHash;

  for (auto const &block : init_blocks) {
    SHA256Hash block_hash = sha256(parent_hash);
    auto res = coord_->validate_block(parent_hash, block, block_hash)();
    ASSERT_TRUE(res);
    parent_hash = block_hash;
  }
}

} // namespace hogd
