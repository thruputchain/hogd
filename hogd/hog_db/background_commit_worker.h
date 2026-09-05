#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

#include "auth_rpc.hh"
#include "hogd/base/perthread.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/rpc_common.h"

namespace hogd {

class BackgroundCommitWorker {
public:
  using sched_db_client_t =
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;
  BackgroundCommitWorker(
      AuthChangeSubscriptionManager &auth_change_subscription_manager,
      std::shared_ptr<sched_db_client_t> sched_db_client);

  void submit_job(BlockHeight block_height,
                  PerThread<KeyChangeBatch> key_changes,
                  PerThread<std::vector<Delta>> balance_deltas);

  ~BackgroundCommitWorker()
  {
    background_worker_.request_stop();
    cv_.notify_all();
  }

private:
  struct Job {
    PerThread<KeyChangeBatch> key_change_batch_;
    PerThread<std::vector<Delta>> balance_deltas_;
    BlockHeight block_height_;
  };

  void commit_worker(std::stop_token &stoken);
  void commit_job(Job &job);

  std::queue<Job> jobs_;

  AuthChangeSubscriptionManager &auth_change_subscription_manager_;
  std::shared_ptr<sched_db_client_t> sched_db_client_;

  std::mutex mutex_;
  std::condition_variable_any cv_;

  std::jthread background_worker_;
};
} // namespace hogd
